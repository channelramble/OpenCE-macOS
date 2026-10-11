/*
HOST_MEMORY.C

The guest's memory: a 4 GB window of the host's address space, at a 4 GB
boundary (port/macos/README.md). It is reserved at start-up, with room on
either side that is never mapped, so that an access a little outside it (a
negative offset from a NULL pointer) faults instead of reaching host memory.

The guest believes in 4 KB pages, as Linux has them (its musl's PAGESIZE, and
the Xbox memory functions, port/linux/src/xbox_memory.c); macOS on Apple
silicon maps 16 KB ones. The window is kept track of in 4 KB guest pages,
and mapped in 16 KB host pages: a mapping that covers a host page entirely
replaces it, one that covers part of it zeroes that part and leaves the
page readable and writable (the rest of the page is someone else's).

Write tracking (port/linux/src/memory_watch.c's interface, through
guest_memory_watch.c) protects the Xbox window's pages and catches the first
write to each, by the host page: a write anywhere in a 16 KB page counts for
all of it. Faults that are not the write tracking's are reported with
guest-relative addresses and passed on to the system.
*/

#include "host.h"
#include "host_watch_hash.h"

#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

uint64_t host_window_base;

#define GUEST_PAGE 0x1000ULL
#define HOST_PAGE 0x4000ULL
#define GUEST_PAGES_PER_HOST_PAGE (HOST_PAGE / GUEST_PAGE)
#define GUEST_PAGE_COUNT (HOST_WINDOW_SIZE / GUEST_PAGE)
#define HOST_PAGE_COUNT (HOST_WINDOW_SIZE / HOST_PAGE)
/* never mapped below the window and above it */
#define GUARD_SIZE 0x4000000ULL
/* nothing is mapped below this guest address: a NULL pointer's offsets */
#define LOW_LIMIT 0x01000000ULL

/* the ranges the pools hand out pages from (guest addresses): around the
Custom Edition tag cache (0x40440000, port/linux/game/cache_file_formats.h),
the Xbox window (0x80000000, 512 MB: port/linux/src/platform.h) and the image
(0xA0000000, port/macos/guest/guest.ld), which the guest maps itself */
static const struct { uint64_t start, end; } pool_ranges[] =
{
	{ 0x01000000ULL, 0x40000000ULL },
	{ 0x44000000ULL, 0x80000000ULL },
	{ 0xB0000000ULL, 0xFFF00000ULL },
};
#define POOL_RANGE_COUNT (sizeof(pool_ranges) / sizeof(pool_ranges[0]))

/* the Xbox window, as PLATFORM_CONTIGUOUS_BASE and _SIZE */
#define XBOX_WINDOW_BASE 0x80000000ULL
#define XBOX_WINDOW_SIZE 0x20000000ULL

/* each guest page's owner */
enum
{
	_page_free,
	/* the pools' (host_low_map, or the guest's mmap without an address) */
	_page_pool,
	/* the rest of a pool allocation's last host page */
	_page_slack,
	/* mapped by the guest where it asked */
	_page_fixed,
	_page_image,
};

static uint8_t page_owner[GUEST_PAGE_COUNT];
static uint64_t pool_cursor[POOL_RANGE_COUNT];
static pthread_mutex_t memory_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t image_start, image_end;

/* Linux's mmap flags and advice, as the guest's musl passes them */
#define LINUX_MAP_SHARED 0x01
#define LINUX_MAP_PRIVATE 0x02
#define LINUX_MAP_FIXED 0x10
#define LINUX_MAP_ANONYMOUS 0x20
#define LINUX_MAP_FIXED_NOREPLACE 0x100000
#define LINUX_MADV_DONTNEED 4
#define LINUX_MADV_FREE 8

#define LINUX_EBADF 9
#define LINUX_ENOMEM 12
#define LINUX_EEXIST 17
#define LINUX_EINVAL 22

static void *host_address(uint64_t guest)
{
	return (void *)(uintptr_t)(host_window_base + guest);
}

static uint64_t round_up(uint64_t value, uint64_t unit)
{
	return (value + unit - 1) & ~(unit - 1);
}

/* ---------- the window */

int host_memory_reserve(void)
{
	/* the window and its guards, anywhere a 4 GB boundary leaves room for
	both: twice the window, and the guards */
	size_t size = 2 * HOST_WINDOW_SIZE + 2 * GUARD_SIZE;
	uint8_t *reservation = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	uint64_t base;

	if (reservation == MAP_FAILED)
	{
		host_logf(HOST_LOG_ERROR, "cannot reserve the guest's window: %s", strerror(errno));
		return -1;
	}
	base = round_up((uint64_t)(uintptr_t)reservation + GUARD_SIZE, HOST_WINDOW_SIZE);
	host_window_base = base;
	host_logf(HOST_LOG_INFO, "guest window at %016llx", (unsigned long long)base);
	return 0;
}

/* the guest pages first..last+count belong to the window and its owners
allow a mapping there (the low pages and the image's never) */
static int range_valid(uint64_t address, uint64_t size)
{
	return size && address >= LOW_LIMIT && address + size <= HOST_WINDOW_SIZE && address + size > address;
}

static int range_touches_image(uint64_t address, uint64_t size)
{
	return address < image_end && address + size > image_start;
}

/* the host page holding the guest page has no owner left (its pages are
free or slack): gives its memory back, as reserved address space */
static void host_page_release_if_unused(uint64_t host_page)
{
	uint64_t first = host_page * GUEST_PAGES_PER_HOST_PAGE, page;

	for (page = first; page < first + GUEST_PAGES_PER_HOST_PAGE; page++)
	{
		if (page_owner[page] != _page_free && page_owner[page] != _page_slack)
			return;
	}
	for (page = first; page < first + GUEST_PAGES_PER_HOST_PAGE; page++)
		page_owner[page] = _page_free;
	mmap(host_address(host_page * HOST_PAGE), HOST_PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
}

/* ---------- the pools */

/* a free run of host pages in a pool range, from its cursor; the guest
address, or 0 */
static uint64_t pool_find(size_t range, uint64_t size)
{
	uint64_t start = pool_ranges[range].start, end = pool_ranges[range].end;
	uint64_t pages = size / GUEST_PAGE;
	int pass;

	for (pass = 0; pass < 2; pass++)
	{
		uint64_t candidate = pass ? start : (pool_cursor[range] ? pool_cursor[range] : start);
		uint64_t limit = pass ? (pool_cursor[range] ? pool_cursor[range] + size : end) : end;

		if (limit > end)
			limit = end;
		while (candidate + size <= limit)
		{
			uint64_t first = candidate / GUEST_PAGE, page;

			for (page = first; page < first + pages; page++)
			{
				if (page_owner[page] != _page_free)
					break;
			}
			if (page == first + pages)
			{
				pool_cursor[range] = candidate + size;
				return candidate;
			}
			candidate = round_up((page + 1) * GUEST_PAGE, HOST_PAGE);
		}
	}
	return 0;
}

/* takes size bytes from the pools (host pages, whole) for owner; the guest
address, or 0 */
static uint64_t pool_take(uint64_t size)
{
	uint64_t length = round_up(size, HOST_PAGE);
	uint64_t used = round_up(size, GUEST_PAGE);
	size_t range;

	for (range = 0; range < POOL_RANGE_COUNT; range++)
	{
		uint64_t address = pool_find(range, length);

		if (address)
		{
			uint64_t first = address / GUEST_PAGE, page;

			for (page = first; page < first + length / GUEST_PAGE; page++)
				page_owner[page] = page < first + used / GUEST_PAGE ? _page_pool : _page_slack;
			return address;
		}
	}
	return 0;
}

void *host_low_map(size_t size, int protection)
{
	uint64_t address;

	if (!size)
		return NULL;
	pthread_mutex_lock(&memory_lock);
	address = pool_take(size);
	pthread_mutex_unlock(&memory_lock);
	if (!address)
		return NULL;
	if (mmap(host_address(address), round_up(size, HOST_PAGE), protection, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0)
		!= host_address(address))
	{
		host_low_unmap(host_address(address), size);
		return NULL;
	}
	return host_address(address);
}

/* frees the guest pages of a range (any owner but the image) */
static void range_free(uint64_t address, uint64_t size)
{
	uint64_t first = address / GUEST_PAGE, last = (address + size - 1) / GUEST_PAGE, page;
	uint64_t host_page;

	for (page = first; page <= last; page++)
	{
		if (page_owner[page] != _page_image)
			page_owner[page] = _page_free;
	}
	for (host_page = first / GUEST_PAGES_PER_HOST_PAGE; host_page <= last / GUEST_PAGES_PER_HOST_PAGE; host_page++)
		host_page_release_if_unused(host_page);
}

void host_low_unmap(void *address, size_t size)
{
	uint64_t guest = (uint64_t)(uintptr_t)address - host_window_base;

	if (!size)
		return;
	pthread_mutex_lock(&memory_lock);
	range_free(guest & ~(GUEST_PAGE - 1), round_up(size, GUEST_PAGE));
	pthread_mutex_unlock(&memory_lock);
}

/* ---------- the image */

int host_memory_map_image(uint32_t code_start, uint32_t code_end, uint32_t data_start, uint32_t data_end)
{
	uint64_t code_length = round_up(code_end - code_start, HOST_PAGE);
	uint64_t data_length = round_up(data_end - data_start, HOST_PAGE);
	void *wanted = host_address(code_start), *code;
	uint64_t page;

	if ((code_start | data_start) & (HOST_PAGE - 1) || data_start < code_start + code_length)
	{
		host_logf(HOST_LOG_ERROR, "the guest image's segments are not on separate host pages");
		return -1;
	}
	/* Executable memory that is not a signed file's is MAP_JIT's, which
	takes no fixed address: the reservation gives the image's range back,
	and the mapping is asked for there */
	munmap(wanted, code_length);
	code = mmap(wanted, code_length, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
	if (code != wanted)
	{
		host_logf(HOST_LOG_ERROR, "cannot map the guest's code at %p (%p: %s)", wanted, code,
			code == MAP_FAILED ? strerror(errno) : "elsewhere");
		if (code != MAP_FAILED)
			munmap(code, code_length);
		return -1;
	}
	pthread_jit_write_protect_np(0);
	if (mmap(host_address(data_start), data_length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0)
		!= host_address(data_start))
	{
		host_logf(HOST_LOG_ERROR, "cannot map the guest's data: %s", strerror(errno));
		return -1;
	}
	image_start = code_start;
	image_end = data_start + data_length;
	for (page = image_start / GUEST_PAGE; page < image_end / GUEST_PAGE; page++)
		page_owner[page] = _page_image;
	return 0;
}

void host_memory_seal_code(void)
{
	pthread_jit_write_protect_np(1);
	sys_icache_invalidate(host_address(image_start), image_end - image_start);
}

/* ---------- the guest's memory system calls */

/* maps fresh pages over the guest range (whole guest pages): a host page
the range covers is replaced; one it covers part of keeps its other pages,
and the range's part of it is zeroed and left readable and writable */
static int range_map_fresh(uint64_t address, uint64_t size, int protection)
{
	uint64_t end = address + size;
	uint64_t host_page;

	for (host_page = address / HOST_PAGE; host_page * HOST_PAGE < end; host_page++)
	{
		uint64_t page_start = host_page * HOST_PAGE, page_end = page_start + HOST_PAGE;
		uint64_t from = address > page_start ? address : page_start;
		uint64_t to = end < page_end ? end : page_end;

		if (from == page_start && to == page_end)
		{
			if (mmap(host_address(page_start), HOST_PAGE, protection, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0)
				!= host_address(page_start))
			{
				return -1;
			}
			continue;
		}
		if (protection == PROT_NONE)
			continue;
		/* (a page nobody else had is mapped first) */
		{
			uint64_t first = page_start / GUEST_PAGE, page;
			int others = 0;

			for (page = first; page < first + GUEST_PAGES_PER_HOST_PAGE; page++)
			{
				if (page_owner[page] != _page_free && page_owner[page] != _page_slack &&
					(page * GUEST_PAGE < from || page * GUEST_PAGE >= to))
				{
					others = 1;
				}
			}
			if (!others)
			{
				if (mmap(host_address(page_start), HOST_PAGE, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != host_address(page_start))
				{
					return -1;
				}
				continue;
			}
		}
		if (mprotect(host_address(page_start), HOST_PAGE, PROT_READ | PROT_WRITE) != 0)
			return -1;
		memset(host_address(from), 0, to - from);
	}
	return 0;
}

static void range_own(uint64_t address, uint64_t size, int owner)
{
	uint64_t page;

	for (page = address / GUEST_PAGE; page < (address + size) / GUEST_PAGE; page++)
		page_owner[page] = (uint8_t)owner;
}

static int range_free_for(uint64_t address, uint64_t size)
{
	uint64_t page;

	for (page = address / GUEST_PAGE; page < (address + size) / GUEST_PAGE; page++)
	{
		if (page_owner[page] != _page_free)
			return 0;
	}
	return 1;
}

/* a file's contents mapped privately: read into fresh pages (the guest's
offsets need not be on host pages) */
static long map_file_copy(uint64_t address, uint64_t size, int protection, int fd, int64_t offset)
{
	uint64_t done = 0;

	while (done < size)
	{
		ssize_t count = pread(fd, host_address(address + done), (size_t)(size - done), offset + (int64_t)done);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		done += (uint64_t)count;
	}
	if (!(protection & PROT_WRITE))
		host_guest_mprotect(address, size, protection);
	return 0;
}

long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset)
{
	uint64_t length = round_up(size, GUEST_PAGE);
	int anonymous = (flags & LINUX_MAP_ANONYMOUS) != 0;
	int map_protection = anonymous ? protection : PROT_READ | PROT_WRITE;
	long result;

	if (!length || (address & (GUEST_PAGE - 1)))
		return -LINUX_EINVAL;
	if (!anonymous && (flags & LINUX_MAP_SHARED) && (protection & PROT_WRITE))
	{
		/* (nothing of the game's writes a file through a mapping) */
		host_logf(HOST_LOG_WARN, "a shared writable file mapping is not supported");
		return -LINUX_EINVAL;
	}
	if (!anonymous && fd < 0)
		return -LINUX_EBADF;
	pthread_mutex_lock(&memory_lock);
	if (flags & (LINUX_MAP_FIXED | LINUX_MAP_FIXED_NOREPLACE))
	{
		if (!range_valid(address, length) || range_touches_image(address, length))
		{
			pthread_mutex_unlock(&memory_lock);
			return (flags & LINUX_MAP_FIXED_NOREPLACE) ? -LINUX_EEXIST : -LINUX_EINVAL;
		}
		if ((flags & LINUX_MAP_FIXED_NOREPLACE) && !range_free_for(address, length))
		{
			pthread_mutex_unlock(&memory_lock);
			return -LINUX_EEXIST;
		}
		if (range_map_fresh(address, length, map_protection) != 0)
		{
			pthread_mutex_unlock(&memory_lock);
			return -LINUX_ENOMEM;
		}
		range_own(address, length, _page_fixed);
	}
	else
	{
		address = pool_take(length);
		if (!address)
		{
			pthread_mutex_unlock(&memory_lock);
			return -LINUX_ENOMEM;
		}
		if (mmap(host_address(address), round_up(length, HOST_PAGE), map_protection,
			MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != host_address(address))
		{
			range_free(address, length);
			pthread_mutex_unlock(&memory_lock);
			return -LINUX_ENOMEM;
		}
	}
	pthread_mutex_unlock(&memory_lock);
	result = (long)address;
	if (!anonymous)
		map_file_copy(address, length, protection, fd, offset);
	return result;
}

long host_guest_munmap(uint64_t address, uint64_t size)
{
	uint64_t length = round_up(size, GUEST_PAGE);
	uint64_t host_page, end;

	if ((address & (GUEST_PAGE - 1)) || !range_valid(address, length))
		return -LINUX_EINVAL;
	if (range_touches_image(address, length))
		return -LINUX_EINVAL;
	pthread_mutex_lock(&memory_lock);
	/* the host pages wholly unmapped become reserved address space again
	(range_free does it for those nobody owns any more); a page left in
	part keeps its memory */
	end = address + length;
	for (host_page = round_up(address, HOST_PAGE) / HOST_PAGE; (host_page + 1) * HOST_PAGE <= end; host_page++)
		mmap(host_address(host_page * HOST_PAGE), HOST_PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
	range_free(address, length);
	pthread_mutex_unlock(&memory_lock);
	return 0;
}

long host_guest_mprotect(uint64_t address, uint64_t size, int protection)
{
	uint64_t end = address + round_up(size, GUEST_PAGE);
	uint64_t host_page;

	if ((address & (GUEST_PAGE - 1)) || !range_valid(address, end - address) || range_touches_image(address, end - address))
		return -LINUX_EINVAL;
	protection &= PROT_READ | PROT_WRITE | PROT_EXEC;
	/* (guest code runs from the image alone) */
	protection &= ~PROT_EXEC;
	for (host_page = address / HOST_PAGE; host_page * HOST_PAGE < end; host_page++)
	{
		uint64_t page_start = host_page * HOST_PAGE;

		if (page_start >= address && page_start + HOST_PAGE <= end)
		{
			if (mprotect(host_address(page_start), HOST_PAGE, protection) != 0)
				return -LINUX_ENOMEM;
		}
		else if (protection != PROT_NONE)
		{
			/* part of a page: the most any of its parts may have */
			mprotect(host_address(page_start), HOST_PAGE, PROT_READ | PROT_WRITE);
		}
	}
	return 0;
}

long host_guest_madvise(uint64_t address, uint64_t size, int advice)
{
	uint64_t end = address + round_up(size, GUEST_PAGE);
	uint64_t host_page;

	if (!range_valid(address, end - address))
		return -LINUX_EINVAL;
	if (advice != LINUX_MADV_DONTNEED && advice != LINUX_MADV_FREE)
		return 0;
	for (host_page = round_up(address, HOST_PAGE) / HOST_PAGE; (host_page + 1) * HOST_PAGE <= end; host_page++)
	{
		/* (Linux's MADV_DONTNEED gives private pages back as zeroes, which
		macOS's does not promise: fresh pages do; the guest's malloc, which
		alone advises, keeps its memory readable and writable) */
		if (advice == LINUX_MADV_DONTNEED)
			mmap(host_address(host_page * HOST_PAGE), HOST_PAGE, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
		else
			madvise(host_address(host_page * HOST_PAGE), HOST_PAGE, MADV_FREE);
	}
	return 0;
}

/* ---------- write tracking (port/linux/src/memory_watch.c)

By the host page (16 KB) under page protection; by the 4 KB page under page
hashes (host_watch_hash.h). */

#define WATCH_PAGE_COUNT (XBOX_WINDOW_SIZE / HOST_PAGE)
#define HASH_PAGE_COUNT (XBOX_WINDOW_SIZE / WATCH_HASH_PAGE_SIZE)

static uint8_t page_protected[WATCH_PAGE_COUNT];
static uint32_t page_generation[WATCH_PAGE_COUNT];
static volatile uint32_t current_generation = 1;
static int watch_active;
static int watch_hashing;
static uint8_t page_hash_watched[HASH_PAGE_COUNT];
static uint64_t page_hash[HASH_PAGE_COUNT];
static uint32_t page_hashed_frame[HASH_PAGE_COUNT];
static uint32_t page_hash_generation[HASH_PAGE_COUNT];
static uint8_t page_hash_unchanged[HASH_PAGE_COUNT];
static struct watch_hash page_hashes =
{
	.page_count = HASH_PAGE_COUNT,
	.watched = page_hash_watched,
	.hash = page_hash,
	.hashed_frame = page_hashed_frame,
	.generation = page_hash_generation,
	.current_generation = &current_generation,
	.frame = 1,
	.unchanged = page_hash_unchanged,
};

static int in_xbox_window(uint64_t guest)
{
	return guest >= XBOX_WINDOW_BASE && guest - XBOX_WINDOW_BASE < XBOX_WINDOW_SIZE;
}

static void mark_written(uint64_t page)
{
	page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	page_protected[page] = 0;
	mprotect(host_address(XBOX_WINDOW_BASE + page * HOST_PAGE), HOST_PAGE, PROT_READ | PROT_WRITE);
}

void host_memory_watch_initialize(void)
{
	page_hashes.base = host_address(XBOX_WINDOW_BASE);
	watch_active = 1;
}

void host_memory_watch_use_hashes(void)
{
	watch_hashing = 1;
}

/* the watched range's pages (host pages, or hash pages) */
static int watch_range(uint32_t address, uint32_t size, uint64_t unit, uint64_t count, uint64_t *first,
	uint64_t *last)
{
	if (!size || !in_xbox_window(address))
		return 0;
	*first = (address - XBOX_WINDOW_BASE) / unit;
	*last = ((uint64_t)address + size - 1 - XBOX_WINDOW_BASE) / unit;
	if (*last >= count)
		*last = count - 1;
	return 1;
}

void host_memory_watch_protect(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!watch_active)
		return;
	if (watch_hashing)
	{
		if (watch_range(address, size, WATCH_HASH_PAGE_SIZE, HASH_PAGE_COUNT, &first, &last))
			watch_hash_protect(&page_hashes, (uint32_t)first, (uint32_t)last);
		return;
	}
	if (!watch_range(address, size, HOST_PAGE, WATCH_PAGE_COUNT, &first, &last))
		return;
	for (page = first; page <= last; page++)
	{
		if (!page_protected[page])
		{
			page_protected[page] = 1;
			mprotect(host_address(XBOX_WINDOW_BASE + page * HOST_PAGE), HOST_PAGE, PROT_READ);
		}
	}
}

uint32_t host_memory_watch_serial(void)
{
	return current_generation;
}

uint32_t host_memory_watch_generation(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;
	uint32_t newest = 0;

	if (watch_hashing)
	{
		if (!watch_range(address, size, WATCH_HASH_PAGE_SIZE, HASH_PAGE_COUNT, &first, &last))
			return 0;
		return watch_hash_generation(&page_hashes, (uint32_t)first, (uint32_t)last);
	}
	if (!watch_range(address, size, HOST_PAGE, WATCH_PAGE_COUNT, &first, &last))
		return 0;
	for (page = first; page <= last; page++)
	{
		if (page_generation[page] > newest)
			newest = page_generation[page];
	}
	return newest;
}

void host_memory_watch_prepare_write(uint32_t address, uint32_t size)
{
	uint64_t start = address, first, last, page;

	if (!watch_active || !size)
		return;
	if (start + size <= XBOX_WINDOW_BASE || start >= XBOX_WINDOW_BASE + XBOX_WINDOW_SIZE)
		return;
	if (start < XBOX_WINDOW_BASE)
	{
		size -= (uint32_t)(XBOX_WINDOW_BASE - start);
		start = XBOX_WINDOW_BASE;
	}
	if (watch_hashing)
	{
		if (watch_range((uint32_t)start, size, WATCH_HASH_PAGE_SIZE, HASH_PAGE_COUNT, &first, &last))
			watch_hash_prepare_write(&page_hashes, (uint32_t)first, (uint32_t)last);
		return;
	}
	if (!watch_range((uint32_t)start, size, HOST_PAGE, WATCH_PAGE_COUNT, &first, &last))
		return;
	for (page = first; page <= last; page++)
	{
		if (page_protected[page])
			mark_written(page);
	}
}

void host_memory_watch_forget(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (watch_hashing)
	{
		if (watch_range(address, size, WATCH_HASH_PAGE_SIZE, HASH_PAGE_COUNT, &first, &last))
			watch_hash_forget(&page_hashes, (uint32_t)first, (uint32_t)last);
		return;
	}
	if (!watch_range(address, size, HOST_PAGE, WATCH_PAGE_COUNT, &first, &last))
		return;
	for (page = first; page <= last; page++)
	{
		/* (a page that stays protected for the rest of it, beyond the
		range, is written to now: the range's new contents may be there) */
		if (page_protected[page])
			mark_written(page);
		page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	}
}

void host_memory_watch_begin_frame(void)
{
	if (watch_hashing)
		watch_hash_begin_frame(&page_hashes);
}

/* ---------- faults */

static struct sigaction previous_segv, previous_bus, previous_ill, previous_trap;

static int guest_owns_pointer(uint64_t address, uint64_t size)
{
	return address >= host_window_base + LOW_LIMIT && address + size <= host_window_base + HOST_WINDOW_SIZE;
}

static void report_crash(int signal_number, siginfo_t *information, void *context)
{
	ucontext_t *ucontext = context;
	const struct __darwin_arm_thread_state64 *state = &ucontext->uc_mcontext->__ss;
	uint64_t pc = state->__pc, lr = state->__lr, fault = (uint64_t)(uintptr_t)information->si_addr;
	int index;

	host_logf(HOST_LOG_ERROR, "signal %d at address %016llx (guest %08llx): pc %016llx lr %016llx sp %016llx",
		signal_number, (unsigned long long)fault, (unsigned long long)(fault - host_window_base),
		(unsigned long long)pc, (unsigned long long)lr, (unsigned long long)state->__sp);
	if (pc >= host_window_base + host_image.base && pc < host_window_base + host_image.end)
	{
		host_logf(HOST_LOG_ERROR, "  in the guest image: llvm-symbolizer --obj=build/macos/halo_guest.elf 0x%llx 0x%llx",
			(unsigned long long)(pc - host_window_base), (unsigned long long)(lr - host_window_base));
	}
	for (index = 0; index < 29; index += 4)
	{
		host_logf(HOST_LOG_ERROR, "  x%-2d %016llx %016llx %016llx %016llx", index,
			(unsigned long long)state->__x[index],
			(unsigned long long)(index + 1 < 29 ? state->__x[index + 1] : state->__fp),
			(unsigned long long)(index + 2 < 29 ? state->__x[index + 2] : state->__lr),
			(unsigned long long)(index + 3 < 29 ? state->__x[index + 3] : state->__sp));
	}
	/* the frame records: the caller's frame pointer, then the return
	address (host addresses; the guest's are their low 32 bits) */
	{
		uint64_t fp = state->__fp;

		for (index = 0; index < 32 && fp && (fp & 7) == 0 && guest_owns_pointer(fp, 16); index++)
		{
			const uint64_t *frame = (const uint64_t *)(uintptr_t)fp;

			host_logf(HOST_LOG_ERROR, "  frame %2d: return %016llx (guest %08x)", index,
				(unsigned long long)frame[1], (uint32_t)frame[1]);
			fp = frame[0];
		}
	}
}

static void chain(struct sigaction *previous, int signal_number, siginfo_t *information, void *context)
{
	sigaction(signal_number, previous, NULL);
	if (previous->sa_flags & SA_SIGINFO)
	{
		if (previous->sa_sigaction)
			previous->sa_sigaction(signal_number, information, context);
	}
	else if (previous->sa_handler != SIG_DFL && previous->sa_handler != SIG_IGN)
	{
		previous->sa_handler(signal_number);
	}
	/* returning runs the faulting instruction again, under the previous
	(or the default) handler */
}

/* a write to a page the write tracking protected: let it through */
static int watch_fault(siginfo_t *information)
{
	uint64_t address = (uint64_t)(uintptr_t)information->si_addr - host_window_base;

	if (watch_active && !watch_hashing && in_xbox_window(address))
	{
		uint64_t page = (address - XBOX_WINDOW_BASE) / HOST_PAGE;

		if (page_protected[page])
		{
			mark_written(page);
			return 1;
		}
	}
	return 0;
}

static void segv_handler(int signal_number, siginfo_t *information, void *context)
{
	if (watch_fault(information))
		return;
	report_crash(signal_number, information, context);
	chain(&previous_segv, signal_number, information, context);
}

/* (macOS reports a write to a read-only page as SIGBUS) */
static void bus_handler(int signal_number, siginfo_t *information, void *context)
{
	if (watch_fault(information))
		return;
	report_crash(signal_number, information, context);
	chain(&previous_bus, signal_number, information, context);
}

static void ill_handler(int signal_number, siginfo_t *information, void *context)
{
	report_crash(signal_number, information, context);
	chain(signal_number == SIGTRAP ? &previous_trap : &previous_ill, signal_number, information, context);
}

void host_install_signal_handlers(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&action.sa_mask);
	action.sa_sigaction = segv_handler;
	sigaction(SIGSEGV, &action, &previous_segv);
	action.sa_sigaction = bus_handler;
	sigaction(SIGBUS, &action, &previous_bus);
	action.sa_sigaction = ill_handler;
	sigaction(SIGILL, &action, &previous_ill);
	sigaction(SIGTRAP, &action, &previous_trap);
}
