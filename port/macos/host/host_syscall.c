/*
HOST_SYSCALL.C

Linux system calls on behalf of the guest's musl runtime (its
syscall_arch.h sends every call here, port/android/guest/runtime), performed
with Darwin's.

The guest's musl is Linux's: its system call numbers, flags, errno values and
structures are the AArch64 kernel's (port/android/guest/libc/arch/arm64_32),
with the ILP32 guest's own layouts where the kernel's would take its longs
and pointers (timespec and timeval with a 32-bit time_t, iovec). Each call
is translated: pointers widened to the window (HOST_POINTER), flags and
constants converted, structures filled in the guest's layout, and the
result's errno given the Linux number. File descriptors are the host's.

Futexes, which musl's locks and condition variables are built on, wait on
the host address of the guest's word (os_sync_wait_on_address, or __ulock
before macOS 14.4). A requeue wakes the waiters instead: they wait again
where they belong, as after any spurious wake.
*/

#include "host.h"
#include "guest_syscall_numbers.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#if __has_include(<os/os_sync_wait_on_address.h>)
#include <os/os_sync_wait_on_address.h>
#define HAVE_OS_SYNC 1
#endif

/* Darwin's futex before os_sync_wait_on_address (libc++'s atomic waits use it) */
extern int __ulock_wait(uint32_t operation, void *address, uint64_t value, uint32_t timeout_microseconds);
extern int __ulock_wake(uint32_t operation, void *address, uint64_t wake_value);
#define UL_COMPARE_AND_WAIT 1
#define ULF_WAKE_ALL 0x00000100

/* ---------- Linux's numbers */

#define LINUX_EPERM 1
#define LINUX_ENOENT 2
#define LINUX_EINTR 4
#define LINUX_EBADF 9
#define LINUX_EAGAIN 11
#define LINUX_ENOMEM 12
#define LINUX_EFAULT 14
#define LINUX_EINVAL 22
#define LINUX_ENOTTY 25
#define LINUX_ERANGE 34
#define LINUX_ENOSYS 38
#define LINUX_ETIMEDOUT 110

#define LINUX_AT_FDCWD (-100)
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100
#define LINUX_AT_REMOVEDIR 0x200
#define LINUX_AT_EACCESS 0x200
#define LINUX_AT_EMPTY_PATH 0x1000

#define LINUX_O_CREAT 0100
#define LINUX_O_EXCL 0200
#define LINUX_O_NOCTTY 0400
#define LINUX_O_TRUNC 01000
#define LINUX_O_APPEND 02000
#define LINUX_O_NONBLOCK 04000
#define LINUX_O_DSYNC 010000
#define LINUX_O_DIRECTORY 040000
#define LINUX_O_NOFOLLOW 0100000
#define LINUX_O_CLOEXEC 02000000
#define LINUX_O_SYNC 04010000

#define LINUX_F_DUPFD 0
#define LINUX_F_GETFD 1
#define LINUX_F_SETFD 2
#define LINUX_F_GETFL 3
#define LINUX_F_SETFL 4
#define LINUX_F_GETLK 5
#define LINUX_F_SETLK 6
#define LINUX_F_SETLKW 7
#define LINUX_F_DUPFD_CLOEXEC 1030

#define LINUX_FUTEX_WAIT 0
#define LINUX_FUTEX_WAKE 1
#define LINUX_FUTEX_REQUEUE 3
#define LINUX_FUTEX_CMP_REQUEUE 4
#define LINUX_FUTEX_WAIT_BITSET 9
#define LINUX_FUTEX_WAKE_BITSET 10
#define LINUX_FUTEX_COMMAND_MASK 0x7f

#define LINUX_TIMER_ABSTIME 1
#define LINUX_UTIME_NOW ((1 << 30) - 1)
#define LINUX_UTIME_OMIT ((1 << 30) - 2)
#define LINUX_RENAME_NOREPLACE 1
#define LINUX_RENAME_EXCHANGE 2
#define LINUX_SIGABRT 6
#define LINUX_RLIMIT_NOFILE 7

int host_linux_errno(int error)
{
	switch (error)
	{
	case 0: return 0;
	case EDEADLK: return 35;
	case EAGAIN: return 11;
	case EINPROGRESS: return 115;
	case EALREADY: return 114;
	case ENOTSOCK: return 88;
	case EDESTADDRREQ: return 89;
	case EMSGSIZE: return 90;
	case EPROTOTYPE: return 91;
	case ENOPROTOOPT: return 92;
	case EPROTONOSUPPORT: return 93;
	case ESOCKTNOSUPPORT: return 94;
	case ENOTSUP: return 95;
	case EOPNOTSUPP: return 95;
	case EPFNOSUPPORT: return 96;
	case EAFNOSUPPORT: return 97;
	case EADDRINUSE: return 98;
	case EADDRNOTAVAIL: return 99;
	case ENETDOWN: return 100;
	case ENETUNREACH: return 101;
	case ENETRESET: return 102;
	case ECONNABORTED: return 103;
	case ECONNRESET: return 104;
	case ENOBUFS: return 105;
	case EISCONN: return 106;
	case ENOTCONN: return 107;
	case ESHUTDOWN: return 108;
	case ETOOMANYREFS: return 109;
	case ETIMEDOUT: return 110;
	case ECONNREFUSED: return 111;
	case ELOOP: return 40;
	case ENAMETOOLONG: return 36;
	case EHOSTDOWN: return 112;
	case EHOSTUNREACH: return 113;
	case ENOTEMPTY: return 39;
	case EUSERS: return 87;
	case EDQUOT: return 122;
	case ESTALE: return 116;
	case EREMOTE: return 66;
	case ENOLCK: return 37;
	case ENOSYS: return 38;
	case EOVERFLOW: return 75;
	case ECANCELED: return 125;
	case EIDRM: return 43;
	case ENOMSG: return 42;
	case EILSEQ: return 84;
	case EBADMSG: return 74;
	case EMULTIHOP: return 72;
	case ENODATA: return 61;
	case ENOLINK: return 67;
	case ENOSR: return 63;
	case ENOSTR: return 60;
	case EPROTO: return 71;
	case ETIME: return 62;
	case ENOTRECOVERABLE: return 131;
	case EOWNERDEAD: return 130;
	default:
		/* (1 to 34 are the same on both) */
		return error > 0 && error <= 34 ? error : LINUX_EINVAL;
	}
}

int host_errno(void)
{
	return host_linux_errno(errno);
}

static long result_of(long value)
{
	return value == -1 ? -host_linux_errno(errno) : value;
}

/* ---------- the guest's structures */

#define GUEST(type, value) HOST_POINTER(type, (uint32_t)(value))

struct guest_timespec
{
	int32_t seconds;
	int32_t nanoseconds;
};

struct guest_iovec
{
	uint32_t base;
	uint32_t length;
};

/* the AArch64 kernel's struct stat (guest/libc/arch/arm64_32/kstat.h) */
struct guest_kstat
{
	uint64_t st_dev;
	uint64_t st_ino;
	uint32_t st_mode;
	uint32_t st_nlink;
	uint32_t st_uid;
	uint32_t st_gid;
	uint64_t st_rdev;
	uint64_t pad;
	int64_t st_size;
	int32_t st_blksize;
	int32_t pad2;
	int64_t st_blocks;
	int64_t st_atime_sec;
	int64_t st_atime_nsec;
	int64_t st_mtime_sec;
	int64_t st_mtime_nsec;
	int64_t st_ctime_sec;
	int64_t st_ctime_nsec;
	uint32_t unused[2];
};

struct guest_dirent64
{
	uint64_t d_ino;
	int64_t d_off;
	uint16_t d_reclen;
	uint8_t d_type;
	char d_name[];
};

struct guest_utsname
{
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
	char domainname[65];
};

struct guest_rlimit
{
	uint64_t current;
	uint64_t maximum;
};

static int timespec_in(uint64_t address, struct timespec *result)
{
	const struct guest_timespec *value = GUEST(const struct guest_timespec *, address);

	if (!(uint32_t)address)
		return 0;
	result->tv_sec = value->seconds;
	result->tv_nsec = value->nanoseconds;
	return 1;
}

static void timespec_out(uint64_t address, const struct timespec *value)
{
	struct guest_timespec *result = GUEST(struct guest_timespec *, address);

	if (!(uint32_t)address)
		return;
	result->seconds = (int32_t)value->tv_sec;
	result->nanoseconds = (int32_t)value->tv_nsec;
}

static int host_directory_fd(int64_t fd)
{
	return (int)fd == LINUX_AT_FDCWD ? AT_FDCWD : (int)fd;
}

static int host_open_flags(int flags)
{
	int result = flags & 3;

	if (flags & LINUX_O_CREAT) result |= O_CREAT;
	if (flags & LINUX_O_EXCL) result |= O_EXCL;
	if (flags & LINUX_O_NOCTTY) result |= O_NOCTTY;
	if (flags & LINUX_O_TRUNC) result |= O_TRUNC;
	if (flags & LINUX_O_APPEND) result |= O_APPEND;
	if (flags & LINUX_O_NONBLOCK) result |= O_NONBLOCK;
	if ((flags & LINUX_O_SYNC) == LINUX_O_SYNC) result |= O_SYNC;
	else if (flags & LINUX_O_DSYNC) result |= O_DSYNC;
	if (flags & LINUX_O_DIRECTORY) result |= O_DIRECTORY;
	if (flags & LINUX_O_NOFOLLOW) result |= O_NOFOLLOW;
	if (flags & LINUX_O_CLOEXEC) result |= O_CLOEXEC;
	return result;
}

/* the status flags F_GETFL gives, as Linux's */
static int linux_status_flags(int flags)
{
	int result = flags & O_ACCMODE;

	if (flags & O_APPEND) result |= LINUX_O_APPEND;
	if (flags & O_NONBLOCK) result |= LINUX_O_NONBLOCK;
	if (flags & O_DSYNC) result |= LINUX_O_DSYNC;
	return result;
}

static void stat_out(uint64_t address, const struct stat *information)
{
	struct guest_kstat *result = GUEST(struct guest_kstat *, address);

	memset(result, 0, sizeof(*result));
	result->st_dev = (uint64_t)information->st_dev;
	result->st_ino = (uint64_t)information->st_ino;
	result->st_mode = information->st_mode;
	result->st_nlink = information->st_nlink;
	result->st_uid = information->st_uid;
	result->st_gid = information->st_gid;
	result->st_rdev = (uint64_t)information->st_rdev;
	result->st_size = information->st_size;
	result->st_blksize = information->st_blksize;
	result->st_blocks = information->st_blocks;
	result->st_atime_sec = information->st_atimespec.tv_sec;
	result->st_atime_nsec = information->st_atimespec.tv_nsec;
	result->st_mtime_sec = information->st_mtimespec.tv_sec;
	result->st_mtime_nsec = information->st_mtimespec.tv_nsec;
	result->st_ctime_sec = information->st_ctimespec.tv_sec;
	result->st_ctime_nsec = information->st_ctimespec.tv_nsec;
}

/* ---------- reads and writes */

static long guest_vector(uint64_t vector, int count, struct iovec *host_vector)
{
	const struct guest_iovec *guest_vector = GUEST(const struct guest_iovec *, vector);
	int index;

	if (count < 0 || count > 64)
		return -LINUX_EINVAL;
	for (index = 0; index < count; index++)
	{
		host_vector[index].iov_base = GUEST(void *, guest_vector[index].base);
		host_vector[index].iov_len = guest_vector[index].length;
	}
	return 0;
}

/* ---------- directories: getdents64 over a DIR of the descriptor's */

#define DIRECTORY_STREAMS 32

static struct
{
	int fd;
	DIR *stream;
} directory_streams[DIRECTORY_STREAMS];
static pthread_mutex_t directory_lock = PTHREAD_MUTEX_INITIALIZER;

static DIR *directory_stream(int fd, int create)
{
	int index, free_index = -1;

	for (index = 0; index < DIRECTORY_STREAMS; index++)
	{
		if (directory_streams[index].stream && directory_streams[index].fd == fd)
			return directory_streams[index].stream;
		if (!directory_streams[index].stream && free_index < 0)
			free_index = index;
	}
	if (!create || free_index < 0)
		return NULL;
	{
		int copy = dup(fd);
		DIR *stream = copy >= 0 ? fdopendir(copy) : NULL;

		if (!stream)
		{
			if (copy >= 0)
				close(copy);
			return NULL;
		}
		directory_streams[free_index].fd = fd;
		directory_streams[free_index].stream = stream;
		return stream;
	}
}

static void directory_close(int fd)
{
	int index;

	pthread_mutex_lock(&directory_lock);
	for (index = 0; index < DIRECTORY_STREAMS; index++)
	{
		if (directory_streams[index].stream && directory_streams[index].fd == fd)
		{
			closedir(directory_streams[index].stream);
			directory_streams[index].stream = NULL;
		}
	}
	pthread_mutex_unlock(&directory_lock);
}

static long guest_getdents64(int fd, uint64_t buffer, uint32_t size)
{
	char *out = GUEST(char *, buffer);
	uint32_t used = 0;
	DIR *stream;

	pthread_mutex_lock(&directory_lock);
	stream = directory_stream(fd, 1);
	if (!stream)
	{
		pthread_mutex_unlock(&directory_lock);
		return -LINUX_EBADF;
	}
	for (;;)
	{
		long position = telldir(stream);
		struct dirent *entry = readdir(stream);
		struct guest_dirent64 *record;
		size_t length, record_size;

		if (!entry)
			break;
		length = strlen(entry->d_name);
		record_size = (offsetof(struct guest_dirent64, d_name) + length + 1 + 7) & ~(size_t)7;
		if (used + record_size > size)
		{
			seekdir(stream, position);
			if (!used)
			{
				pthread_mutex_unlock(&directory_lock);
				return -LINUX_EINVAL;
			}
			break;
		}
		record = (struct guest_dirent64 *)(out + used);
		record->d_ino = entry->d_ino;
		record->d_off = telldir(stream);
		record->d_reclen = (uint16_t)record_size;
		/* (the DT_ values are the same) */
		record->d_type = entry->d_type;
		memcpy(record->d_name, entry->d_name, length + 1);
		used += (uint32_t)record_size;
	}
	pthread_mutex_unlock(&directory_lock);
	return used;
}

/* ---------- time */

static clockid_t host_clock(int64_t clock)
{
	switch ((int)clock)
	{
	case 1: case 6: case 7: return CLOCK_MONOTONIC;
	case 2: return CLOCK_PROCESS_CPUTIME_ID;
	case 3: return CLOCK_THREAD_CPUTIME_ID;
	case 4: return CLOCK_MONOTONIC_RAW;
	default: return CLOCK_REALTIME;
	}
}

static long sleep_until(clockid_t clock, const struct timespec *deadline)
{
	for (;;)
	{
		struct timespec now, delay;

		clock_gettime(clock, &now);
		delay.tv_sec = deadline->tv_sec - now.tv_sec;
		delay.tv_nsec = deadline->tv_nsec - now.tv_nsec;
		if (delay.tv_nsec < 0)
		{
			delay.tv_nsec += 1000000000L;
			delay.tv_sec--;
		}
		if (delay.tv_sec < 0)
			return 0;
		if (nanosleep(&delay, NULL) == 0)
			return 0;
		if (errno != EINTR)
			return -host_linux_errno(errno);
	}
}

/* ---------- futexes */

static long futex_wait(uint32_t *address, uint32_t value, const struct timespec *timeout)
{
	int result;

	if (__atomic_load_n(address, __ATOMIC_SEQ_CST) != value)
		return -LINUX_EAGAIN;
#ifdef HAVE_OS_SYNC
	if (__builtin_available(macOS 14.4, *))
	{
		if (timeout)
		{
			uint64_t nanoseconds = (uint64_t)timeout->tv_sec * 1000000000ULL + (uint64_t)timeout->tv_nsec;

			if (!nanoseconds)
				return -LINUX_ETIMEDOUT;
			result = os_sync_wait_on_address_with_timeout(address, value, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE,
				OS_CLOCK_MACH_ABSOLUTE_TIME, nanoseconds);
		}
		else
		{
			result = os_sync_wait_on_address(address, value, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE);
		}
		if (result < 0)
			return errno == ETIMEDOUT ? -LINUX_ETIMEDOUT : errno == EINTR ? -LINUX_EINTR : 0;
		return 0;
	}
#endif
	{
		uint64_t microseconds = 0;

		if (timeout)
		{
			microseconds = (uint64_t)timeout->tv_sec * 1000000ULL + (uint64_t)timeout->tv_nsec / 1000;
			if (!microseconds)
				return -LINUX_ETIMEDOUT;
			if (microseconds > 0xffffffffULL)
				microseconds = 0xffffffffULL;
		}
		result = __ulock_wait(UL_COMPARE_AND_WAIT, address, value, (uint32_t)microseconds);
		if (result < 0)
			return errno == ETIMEDOUT ? -LINUX_ETIMEDOUT : errno == EINTR ? -LINUX_EINTR : 0;
		return 0;
	}
}

static long futex_wake(uint32_t *address, uint32_t count)
{
	int result;

	if (!count)
		return 0;
#ifdef HAVE_OS_SYNC
	if (__builtin_available(macOS 14.4, *))
	{
		result = count == 1 ? os_sync_wake_by_address_any(address, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE) :
			os_sync_wake_by_address_all(address, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
		return result == 0 ? 1 : 0;
	}
#endif
	result = __ulock_wake(UL_COMPARE_AND_WAIT | (count == 1 ? 0 : ULF_WAKE_ALL), address, 0);
	return result == 0 ? 1 : 0;
}

static long guest_futex(uint64_t address, int operation, uint32_t value, uint64_t timeout_address,
	uint64_t address2, uint32_t value3)
{
	uint32_t *word = GUEST(uint32_t *, address);
	int command = operation & LINUX_FUTEX_COMMAND_MASK;
	struct timespec timeout;

	if (!word || ((uintptr_t)word & 3))
		return -LINUX_EINVAL;
	switch (command)
	{
	case LINUX_FUTEX_WAIT:
		return futex_wait(word, value, timespec_in(timeout_address, &timeout) ? &timeout : NULL);
	case LINUX_FUTEX_WAIT_BITSET:
		/* (an absolute timeout: made relative) */
		if (timespec_in(timeout_address, &timeout))
		{
			struct timespec now;

			clock_gettime((operation & 256) ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
			timeout.tv_sec -= now.tv_sec;
			timeout.tv_nsec -= now.tv_nsec;
			if (timeout.tv_nsec < 0)
			{
				timeout.tv_nsec += 1000000000L;
				timeout.tv_sec--;
			}
			if (timeout.tv_sec < 0)
				return -LINUX_ETIMEDOUT;
			return futex_wait(word, value, &timeout);
		}
		return futex_wait(word, value, NULL);
	case LINUX_FUTEX_WAKE:
	case LINUX_FUTEX_WAKE_BITSET:
		return futex_wake(word, value);
	case LINUX_FUTEX_CMP_REQUEUE:
		if (__atomic_load_n(word, __ATOMIC_SEQ_CST) != value3)
			return -LINUX_EAGAIN;
		/* fall through */
	case LINUX_FUTEX_REQUEUE:
		(void)address2;
		return futex_wake(word, 0x7fffffff);
	default:
		host_logf(HOST_LOG_WARN, "guest futex operation %d is not supported", operation);
		return -LINUX_ENOSYS;
	}
}

/* ---------- threads */

static int next_thread_id = 1000;
static __thread int thread_id;

static int guest_thread_id(void)
{
	if (!thread_id)
		thread_id = __atomic_add_fetch(&next_thread_id, 1, __ATOMIC_SEQ_CST);
	return thread_id;
}

void host_syscall_thread_exited(void)
{
	thread_id = 0;
}

/* ---------- the rest */

/* the executable's path as the guest asks for it (/proc/self/exe): in the
data folder, where the desktop ports look for maps/ beside the executable
and offer to extract them (port/linux/src/xbox_files.c) */
static long guest_readlink(int fd, const char *path, char *buffer, size_t size)
{
	if (!strcmp(path, "/proc/self/exe"))
	{
		char executable[1100];
		size_t length;

		snprintf(executable, sizeof(executable), "%s/halo", host_data_root());
		length = strlen(executable);
		if (length > size)
			length = size;
		memcpy(buffer, executable, length);
		return (long)length;
	}
	return result_of(readlinkat(fd, path, buffer, size));
}

static long guest_fstatat(int64_t fd, uint64_t path_address, uint64_t result, int flags)
{
	const char *path = GUEST(const char *, path_address);
	struct stat information;
	int host_flags = (flags & LINUX_AT_SYMLINK_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW : 0;
	int value;

	if ((flags & LINUX_AT_EMPTY_PATH) && (!path || !*path))
		value = fstat((int)fd, &information);
	else
		value = fstatat(host_directory_fd(fd), path, &information, host_flags);
	if (value != 0)
		return -host_linux_errno(errno);
	stat_out(result, &information);
	return 0;
}

static long guest_fcntl(int fd, int command, uint64_t argument)
{
	switch (command)
	{
	case LINUX_F_DUPFD:
		return result_of(fcntl(fd, F_DUPFD, (int)argument));
	case LINUX_F_DUPFD_CLOEXEC:
		return result_of(fcntl(fd, F_DUPFD_CLOEXEC, (int)argument));
	case LINUX_F_GETFD:
		return result_of(fcntl(fd, F_GETFD));
	case LINUX_F_SETFD:
		return result_of(fcntl(fd, F_SETFD, (int)argument & FD_CLOEXEC));
	case LINUX_F_GETFL:
	{
		int flags = fcntl(fd, F_GETFL);

		return flags < 0 ? -host_linux_errno(errno) : linux_status_flags(flags);
	}
	case LINUX_F_SETFL:
		return result_of(fcntl(fd, F_SETFL, host_open_flags((int)argument & ~3) & (O_APPEND | O_NONBLOCK)));
	case LINUX_F_GETLK:
	case LINUX_F_SETLK:
	case LINUX_F_SETLKW:
		/* (no record locks in the game) */
		return 0;
	default:
		return -LINUX_EINVAL;
	}
}

static long guest_utimensat(int64_t fd, uint64_t path_address, uint64_t times_address, int flags)
{
	struct timespec times[2];
	const struct guest_timespec *guest_times = GUEST(const struct guest_timespec *, times_address);
	int index;

	if (guest_times)
	{
		for (index = 0; index < 2; index++)
		{
			times[index].tv_sec = guest_times[index].seconds;
			times[index].tv_nsec = guest_times[index].nanoseconds == LINUX_UTIME_NOW ? UTIME_NOW :
				guest_times[index].nanoseconds == LINUX_UTIME_OMIT ? UTIME_OMIT : guest_times[index].nanoseconds;
		}
	}
	if (!(uint32_t)path_address)
		return result_of(futimens((int)fd, guest_times ? times : NULL));
	return result_of(utimensat(host_directory_fd(fd), GUEST(const char *, path_address), guest_times ? times : NULL,
		(flags & LINUX_AT_SYMLINK_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW : 0));
}

static long guest_ppoll(uint64_t fds_address, uint32_t count, uint64_t timeout_address)
{
	struct pollfd *guest_fds = GUEST(struct pollfd *, fds_address);
	struct pollfd local[64];
	struct timespec timeout;
	int milliseconds = -1;
	uint32_t index;
	int result;

	if (count > 64)
		return -LINUX_EINVAL;
	if (timespec_in(timeout_address, &timeout))
		milliseconds = (int)(timeout.tv_sec * 1000 + timeout.tv_nsec / 1000000);
	for (index = 0; index < count; index++)
	{
		short events = guest_fds[index].events;

		local[index].fd = guest_fds[index].fd;
		/* (Linux's POLLWRNORM and POLLWRBAND are Darwin's POLLOUT and
		POLLWRBAND; the others are the same) */
		local[index].events = (short)((events & 0xff) | ((events & 0x100) ? POLLWRNORM : 0) |
			((events & 0x200) ? POLLWRBAND : 0));
		local[index].revents = 0;
	}
	result = poll(local, count, milliseconds);
	if (result < 0)
		return -host_linux_errno(errno);
	for (index = 0; index < count; index++)
		guest_fds[index].revents = (short)((local[index].revents & 0xfb) | ((local[index].revents & POLLOUT) ? 4 : 0) |
			((local[index].revents & POLLWRBAND) ? 0x200 : 0));
	return result;
}

static long guest_uname(uint64_t address)
{
	struct guest_utsname *result = GUEST(struct guest_utsname *, address);
	struct utsname host;

	if (uname(&host) != 0)
		return -host_linux_errno(errno);
	memset(result, 0, sizeof(*result));
	snprintf(result->sysname, sizeof(result->sysname), "%s", host.sysname);
	snprintf(result->nodename, sizeof(result->nodename), "%s", host.nodename);
	snprintf(result->release, sizeof(result->release), "%s", host.release);
	snprintf(result->version, sizeof(result->version), "%s", host.version);
	snprintf(result->machine, sizeof(result->machine), "%s", host.machine);
	return 0;
}

static long guest_prlimit(int resource, uint64_t old_address)
{
	struct guest_rlimit *result = GUEST(struct guest_rlimit *, old_address);

	if (!result)
		return 0;
	result->current = result->maximum = ~0ULL;
	if (resource == LINUX_RLIMIT_NOFILE)
	{
		struct rlimit limit;

		if (getrlimit(RLIMIT_NOFILE, &limit) == 0)
		{
			result->current = limit.rlim_cur == RLIM_INFINITY ? ~0ULL : (uint64_t)limit.rlim_cur;
			result->maximum = limit.rlim_max == RLIM_INFINITY ? ~0ULL : (uint64_t)limit.rlim_max;
		}
	}
	return 0;
}

static long guest_affinity(uint32_t size, uint64_t mask_address)
{
	unsigned char *mask = GUEST(unsigned char *, mask_address);
	int processors = (int)sysconf(_SC_NPROCESSORS_ONLN), index;

	if (!mask || size < 8)
		return -LINUX_EINVAL;
	if (size > 128)
		size = 128;
	memset(mask, 0, size);
	for (index = 0; index < processors && index < (int)size * 8; index++)
		mask[index / 8] |= (unsigned char)(1 << (index % 8));
	return size;
}

static long guest_kill(int signal_number)
{
	if (!signal_number)
		return 0;
	if (signal_number == LINUX_SIGABRT)
	{
		host_logf(HOST_LOG_ERROR, "the game aborted");
		abort();
	}
	host_logf(HOST_LOG_WARN, "the game sent itself signal %d: ignored", signal_number);
	return 0;
}

/* ---------- dispatch */

long long host_syscall(long long number, long long a, long long b, long long c,
	long long d, long long e, long long f)
{
	switch (number)
	{
	case GUEST_SYS_read:
		return result_of(read((int)a, GUEST(void *, b), (size_t)(uint32_t)c));
	case GUEST_SYS_write:
		return result_of(write((int)a, GUEST(const void *, b), (size_t)(uint32_t)c));
	case GUEST_SYS_pread64:
		return result_of(pread((int)a, GUEST(void *, b), (size_t)(uint32_t)c, (off_t)d));
	case GUEST_SYS_pwrite64:
		return result_of(pwrite((int)a, GUEST(const void *, b), (size_t)(uint32_t)c, (off_t)d));
	case GUEST_SYS_readv:
	case GUEST_SYS_writev:
	case GUEST_SYS_preadv:
	case GUEST_SYS_pwritev:
	{
		struct iovec vector[64];
		long error = guest_vector((uint64_t)b, (int)c, vector);

		if (error)
			return error;
		if (number == GUEST_SYS_readv)
			return result_of(readv((int)a, vector, (int)c));
		if (number == GUEST_SYS_writev)
			return result_of(writev((int)a, vector, (int)c));
		if (number == GUEST_SYS_preadv)
			return result_of(preadv((int)a, vector, (int)c, (off_t)d));
		return result_of(pwritev((int)a, vector, (int)c, (off_t)d));
	}
	case GUEST_SYS_openat:
		return result_of(openat(host_directory_fd(a), GUEST(const char *, b), host_open_flags((int)c), (int)d));
	case GUEST_SYS_close:
		directory_close((int)a);
		return result_of(close((int)a));
	case GUEST_SYS_lseek:
	{
		DIR *stream;

		pthread_mutex_lock(&directory_lock);
		stream = directory_stream((int)a, 0);
		if (stream && b == 0 && c == SEEK_SET)
			rewinddir(stream);
		pthread_mutex_unlock(&directory_lock);
		return result_of((long)lseek((int)a, (off_t)b, (int)c));
	}
	case GUEST_SYS_getdents64:
		return guest_getdents64((int)a, (uint64_t)b, (uint32_t)c);
	case GUEST_SYS_fstat:
	{
		struct stat information;

		if (fstat((int)a, &information) != 0)
			return -host_linux_errno(errno);
		stat_out((uint64_t)b, &information);
		return 0;
	}
	case GUEST_SYS_newfstatat:
		return guest_fstatat(a, (uint64_t)b, (uint64_t)c, (int)d);
	case GUEST_SYS_readlinkat:
		return guest_readlink(host_directory_fd(a), GUEST(const char *, b), GUEST(char *, c), (size_t)(uint32_t)d);
	case GUEST_SYS_faccessat:
	case GUEST_SYS_faccessat2:
		return result_of(faccessat(host_directory_fd(a), GUEST(const char *, b), (int)c,
			(number == GUEST_SYS_faccessat2 && (d & LINUX_AT_EACCESS)) ? AT_EACCESS : 0));
	case GUEST_SYS_unlinkat:
		return result_of(unlinkat(host_directory_fd(a), GUEST(const char *, b),
			(c & LINUX_AT_REMOVEDIR) ? AT_REMOVEDIR : 0));
	case GUEST_SYS_renameat:
		return result_of(renameat(host_directory_fd(a), GUEST(const char *, b), host_directory_fd(c),
			GUEST(const char *, d)));
	case GUEST_SYS_renameat2:
		if (!e)
			return result_of(renameat(host_directory_fd(a), GUEST(const char *, b), host_directory_fd(c),
				GUEST(const char *, d)));
		return result_of(renameatx_np(host_directory_fd(a), GUEST(const char *, b), host_directory_fd(c),
			GUEST(const char *, d), (e & LINUX_RENAME_NOREPLACE) ? RENAME_EXCL :
			(e & LINUX_RENAME_EXCHANGE) ? RENAME_SWAP : 0));
	case GUEST_SYS_mkdirat:
		return result_of(mkdirat(host_directory_fd(a), GUEST(const char *, b), (mode_t)c));
	case GUEST_SYS_fchmod:
		return result_of(fchmod((int)a, (mode_t)b));
	case GUEST_SYS_fchmodat:
		return result_of(fchmodat(host_directory_fd(a), GUEST(const char *, b), (mode_t)c, 0));
	case GUEST_SYS_ftruncate:
		return result_of(ftruncate((int)a, (off_t)b));
	case GUEST_SYS_fsync:
	case GUEST_SYS_fdatasync:
		return result_of(fsync((int)a));
	case GUEST_SYS_fcntl:
		return guest_fcntl((int)a, (int)b, (uint64_t)c);
	case GUEST_SYS_flock:
		return result_of(flock((int)a, (int)b));
	case GUEST_SYS_dup:
		return result_of(dup((int)a));
	case GUEST_SYS_dup3:
	{
		int result = dup2((int)a, (int)b);

		if (result >= 0 && (c & LINUX_O_CLOEXEC))
			fcntl(result, F_SETFD, FD_CLOEXEC);
		return result_of(result);
	}
	case GUEST_SYS_pipe2:
	{
		int *fds = GUEST(int *, a), pair[2];

		if (pipe(pair) != 0)
			return -host_linux_errno(errno);
		if (b & LINUX_O_CLOEXEC)
		{
			fcntl(pair[0], F_SETFD, FD_CLOEXEC);
			fcntl(pair[1], F_SETFD, FD_CLOEXEC);
		}
		if (b & LINUX_O_NONBLOCK)
		{
			fcntl(pair[0], F_SETFL, O_NONBLOCK);
			fcntl(pair[1], F_SETFL, O_NONBLOCK);
		}
		fds[0] = pair[0];
		fds[1] = pair[1];
		return 0;
	}
	case GUEST_SYS_getcwd:
	{
		char *buffer = GUEST(char *, a);

		if (!getcwd(buffer, (size_t)(uint32_t)b))
			return -host_linux_errno(errno);
		return (long long)strlen(buffer) + 1;
	}
	case GUEST_SYS_chdir:
		return result_of(chdir(GUEST(const char *, a)));
	case GUEST_SYS_umask:
		return umask((mode_t)a);
	case GUEST_SYS_utimensat:
		return guest_utimensat(a, (uint64_t)b, (uint64_t)c, (int)d);

	case GUEST_SYS_clock_gettime:
	case GUEST_SYS_clock_getres:
	{
		struct timespec value;
		int result = number == GUEST_SYS_clock_gettime ? clock_gettime(host_clock(a), &value) :
			clock_getres(host_clock(a), &value);

		if (result != 0)
			return -host_linux_errno(errno);
		timespec_out((uint64_t)b, &value);
		return 0;
	}
	case GUEST_SYS_gettimeofday:
	{
		struct timespec value;
		struct guest_timespec *result = GUEST(struct guest_timespec *, a);

		clock_gettime(CLOCK_REALTIME, &value);
		if (result)
		{
			result->seconds = (int32_t)value.tv_sec;
			result->nanoseconds = (int32_t)(value.tv_nsec / 1000);
		}
		return 0;
	}
	case GUEST_SYS_nanosleep:
	{
		struct timespec request, remaining;
		long result;

		if (!timespec_in((uint64_t)a, &request))
			return -LINUX_EFAULT;
		result = result_of(nanosleep(&request, &remaining));
		if (result == -LINUX_EINTR)
			timespec_out((uint64_t)b, &remaining);
		return result;
	}
	case GUEST_SYS_clock_nanosleep:
	{
		struct timespec request;

		if (!timespec_in((uint64_t)c, &request))
			return -LINUX_EFAULT;
		if (b & LINUX_TIMER_ABSTIME)
			return sleep_until(host_clock(a), &request);
		while (nanosleep(&request, &request) != 0)
		{
			if (errno != EINTR)
				return -host_linux_errno(errno);
		}
		return 0;
	}
	case GUEST_SYS_futex:
		return guest_futex((uint64_t)a, (int)b, (uint32_t)c, (uint64_t)d, (uint64_t)e, (uint32_t)f);
	case GUEST_SYS_ppoll:
		return guest_ppoll((uint64_t)a, (uint32_t)b, (uint64_t)c);
	case GUEST_SYS_sched_yield:
		sched_yield();
		return 0;

	case GUEST_SYS_mmap:
		return host_guest_mmap((uint32_t)a, (uint64_t)b, (int)c, (int)d, (int)e, f);
	case GUEST_SYS_munmap:
		return host_guest_munmap((uint32_t)a, (uint64_t)b);
	case GUEST_SYS_mprotect:
		return host_guest_mprotect((uint32_t)a, (uint64_t)b, (int)c);
	case GUEST_SYS_madvise:
		return host_guest_madvise((uint32_t)a, (uint64_t)b, (int)c);
	case GUEST_SYS_mremap:
	case GUEST_SYS_brk:
		/* musl then falls back to mmap and copying */
		return -LINUX_ENOMEM;

	case GUEST_SYS_exit:
	case GUEST_SYS_exit_group:
		host_exit((int)a);

	case GUEST_SYS_set_tid_address:
	case GUEST_SYS_gettid:
		return guest_thread_id();
	case GUEST_SYS_getpid:
		return getpid();
	case GUEST_SYS_getppid:
		return getppid();
	case GUEST_SYS_getuid:
		return getuid();
	case GUEST_SYS_geteuid:
		return geteuid();
	case GUEST_SYS_getgid:
		return getgid();
	case GUEST_SYS_getegid:
		return getegid();
	case GUEST_SYS_getrandom:
		arc4random_buf(GUEST(void *, a), (size_t)(uint32_t)b);
		return (uint32_t)b;
	case GUEST_SYS_uname:
		return guest_uname((uint64_t)a);
	case GUEST_SYS_prlimit64:
		return guest_prlimit((int)b, (uint64_t)d);
	case GUEST_SYS_sched_getaffinity:
		return guest_affinity((uint32_t)b, (uint64_t)c);
	case GUEST_SYS_kill:
		return guest_kill((int)b);
	case GUEST_SYS_tkill:
		return guest_kill((int)b);
	case GUEST_SYS_tgkill:
		return guest_kill((int)c);

	case GUEST_SYS_rt_sigaction:
	case GUEST_SYS_sigaltstack:
	case GUEST_SYS_membarrier:
	case GUEST_SYS_prctl:
	case GUEST_SYS_set_robust_list:
		/* the host owns signal handling; the rest have nothing to do */
		return 0;
	case GUEST_SYS_rt_sigprocmask:
		if ((uint32_t)c)
			memset(GUEST(void *, c), 0, (size_t)(uint32_t)d);
		return 0;
	case GUEST_SYS_ioctl:
		return -LINUX_ENOTTY;
	case GUEST_SYS_getrlimit:
	case GUEST_SYS_statx:
	case GUEST_SYS_statfs:
	case GUEST_SYS_fstatfs:
	case GUEST_SYS_clone:
	case GUEST_SYS_clone3:
	case GUEST_SYS_execve:
	case GUEST_SYS_rt_sigtimedwait:
	case GUEST_SYS_pselect6:
	case GUEST_SYS_epoll_pwait:
	case GUEST_SYS_sysinfo:
	case GUEST_SYS_sendmsg:
	case GUEST_SYS_recvmsg:
	case GUEST_SYS_timer_create:
	case GUEST_SYS_timer_settime:
	case GUEST_SYS_timer_gettime:
	case GUEST_SYS_setitimer:
	case GUEST_SYS_getitimer:
	case GUEST_SYS_times:
	case GUEST_SYS_getrusage:
	case GUEST_SYS_wait4:
	case GUEST_SYS_waitid:
		return -LINUX_ENOSYS;

	default:
		host_logf(HOST_LOG_WARN, "guest system call %lld is not supported", number);
		return -LINUX_ENOSYS;
	}
}
