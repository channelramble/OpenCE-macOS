/*
HOST_MAIN.C

Entry point of the macOS port (port/macos/README.md).

It reserves the guest's window, loads the guest image (the game, built as
ILP32 code and rewritten for the window) from the app's resources, gives it
an environment and runs its main() on the process's main thread, which
SDL's windows and events need, moved to a stack in the window.

The app itself is read-only (and signed), so what the desktop ports keep
beside their executable lives in the data folder,
~/Library/Application Support/OpenCE (or $HALO_DATA_ROOT): config.toml,
maps/ (which the first start offers to extract from a disc image), the
game's log, debug.txt, and the saved games, in save/. The guest sees itself
as an executable in that folder (host_syscall.c, /proc/self/exe), and it is
the folder the guest starts in.
*/

#include "host.h"

#include <SDL3/SDL.h>
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---------- logging and termination */

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

void host_logf(int priority, const char *format, ...)
{
	va_list arguments;

	pthread_mutex_lock(&log_lock);
	fputs(priority >= HOST_LOG_ERROR ? "halo: error: " : priority >= HOST_LOG_WARN ? "halo: warning: " : "halo: ",
		stderr);
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
	fflush(stderr);
	pthread_mutex_unlock(&log_lock);
}

void host_log(int priority, const char *text)
{
	host_logf(priority, "%s", text);
}

void host_fatal(const char *format, ...)
{
	char message[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	host_logf(HOST_LOG_ERROR, "%s", message);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Halo", message, NULL);
	_exit(1);
}

void host_abort(const char *reason)
{
	host_logf(HOST_LOG_ERROR, "guest abort: %s", reason);
	abort();
}

void host_exit(int code)
{
	host_logf(HOST_LOG_INFO, "the game exited (%d)", code);
	fflush(NULL);
	/* (a fullscreen display mode and the pointer go back as they were) */
	if (pthread_main_np())
		SDL_Quit();
	_exit(code);
}

/* ---------- paths */

static char data_root[PATH_MAX];
static char resources[PATH_MAX];
static char frameworks[PATH_MAX];

const char *host_data_root(void)
{
	return data_root;
}

const char *host_library_path(const char *name)
{
	static __thread char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s", frameworks, name);
	return path;
}

/* the app's folders: Contents/Resources and Contents/Frameworks of the
bundle holding the executable, or, for the executable that the build
leaves in build/macos/host, the build's own */
static void find_app_folders(void)
{
	char executable[PATH_MAX], resolved[PATH_MAX];
	uint32_t size = sizeof(executable);
	char *slash;

	if (_NSGetExecutablePath(executable, &size) != 0 || !realpath(executable, resolved))
		host_fatal("cannot find the game's executable");
	slash = strrchr(resolved, '/');
	if (slash)
		*slash = 0;
	if (strlen(resolved) > 15 && !strcmp(resolved + strlen(resolved) - 15, "/Contents/MacOS"))
	{
		resolved[strlen(resolved) - 6] = 0;
		snprintf(resources, sizeof(resources), "%s/Resources", resolved);
		snprintf(frameworks, sizeof(frameworks), "%s/Frameworks", resolved);
	}
	else
	{
		/* build/macos/host/halo: the image in build/macos, SDL3 and ANGLE
		where the build put them */
		slash = strrchr(resolved, '/');
		if (slash)
			*slash = 0;
		snprintf(resources, sizeof(resources), "%s", resolved);
		snprintf(frameworks, sizeof(frameworks), "%s/Halo.app/Contents/Frameworks", resolved);
	}
}

static void make_directories(const char *path)
{
	char partial[PATH_MAX];
	size_t index;

	snprintf(partial, sizeof(partial), "%s", path);
	for (index = 1; partial[index]; index++)
	{
		if (partial[index] == '/')
		{
			partial[index] = 0;
			mkdir(partial, 0755);
			partial[index] = '/';
		}
	}
	mkdir(partial, 0755);
}

static void find_data_root(void)
{
	const char *chosen = getenv("HALO_DATA_ROOT");
	const char *home = getenv("HOME");

	if (chosen && *chosen)
		snprintf(data_root, sizeof(data_root), "%s", chosen);
	else if (home && *home)
		snprintf(data_root, sizeof(data_root), "%s/Library/Application Support/OpenCE", home);
	else
		host_fatal("HOME is not set: cannot find the game's data folder");
	while (strlen(data_root) > 1 && data_root[strlen(data_root) - 1] == '/')
		data_root[strlen(data_root) - 1] = 0;
	make_directories(data_root);
}

/* started from the Finder (no terminal): the logs go to halo.log in the data
folder, as the Windows build's do beside it */
static void open_log(void)
{
	char path[PATH_MAX];

	if (isatty(STDERR_FILENO))
		return;
	snprintf(path, sizeof(path), "%s/halo.log", data_root);
	if (freopen(path, "w", stderr))
	{
		dup2(STDERR_FILENO, STDOUT_FILENO);
		setvbuf(stderr, NULL, _IOLBF, 0);
	}
}

static void *read_file(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	void *data = NULL;
	long length;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > 0 && fseek(file, 0, SEEK_SET) == 0)
	{
		data = malloc((size_t)length);
		if (data && fread(data, 1, (size_t)length, file) != (size_t)length)
		{
			free(data);
			data = NULL;
		}
		*size = (size_t)length;
	}
	fclose(file);
	return data;
}

/* internet play's MQTT brokers (network.brokers_file): the app's list,
written beside config.toml at each start, as a desktop update replaces the
file beside its game */
static void write_brokers(void)
{
	char source[PATH_MAX], destination[PATH_MAX];
	size_t size = 0;
	void *brokers;
	FILE *file;

	snprintf(source, sizeof(source), "%s/brokers.txt", resources);
	snprintf(destination, sizeof(destination), "%s/brokers.txt", data_root);
	brokers = read_file(source, &size);
	if (!brokers)
	{
		host_logf(HOST_LOG_WARN, "cannot read %s", source);
		return;
	}
	file = fopen(destination, "wb");
	if (!file || fwrite(brokers, 1, size, file) != size)
		host_logf(HOST_LOG_ERROR, "cannot write %s", destination);
	if (file)
		fclose(file);
	free(brokers);
}

/* ---------- the guest's environment */

#define ENVIRONMENT_MAXIMUM 128
#define ARGUMENT_MAXIMUM 16

struct environment
{
	char *entries[ENVIRONMENT_MAXIMUM];
	int count;
};

static void environment_set(struct environment *environment, const char *name, const char *value)
{
	size_t length = strlen(name);
	char *entry = malloc(length + strlen(value) + 2);
	int index;

	sprintf(entry, "%s=%s", name, value);
	for (index = 0; index < environment->count; index++)
	{
		if (!strncmp(environment->entries[index], name, length) && environment->entries[index][length] == '=')
		{
			free(environment->entries[index]);
			environment->entries[index] = entry;
			return;
		}
	}
	if (environment->count < ENVIRONMENT_MAXIMUM)
		environment->entries[environment->count++] = entry;
	else
		free(entry);
}

/* POSIX TZ for the current local offset (the guest's musl has no zone
database) */
static void time_zone(char *buffer, size_t size)
{
	time_t now = time(NULL);
	struct tm local;
	long offset;

	localtime_r(&now, &local);
	offset = -local.tm_gmtoff;
	snprintf(buffer, size, "<L>%s%ld:%02ld", offset < 0 ? "-" : "", labs(offset) / 3600, (labs(offset) / 60) % 60);
}

/* copies argv and the environment into guest memory */
static uint32_t make_boot(int argc, char **argv, const struct environment *environment)
{
	size_t size = 0x10000;
	char *memory = host_low_map(size, PROT_READ | PROT_WRITE);
	struct halo_guest_boot *boot = (struct halo_guest_boot *)memory;
	uint32_t *guest_argv = (uint32_t *)(memory + sizeof(*boot));
	uint32_t *environ_list = guest_argv + ARGUMENT_MAXIMUM + 1;
	char *strings = (char *)(environ_list + ENVIRONMENT_MAXIMUM + 1);
	int index, count = 0;

	if (!memory)
		host_fatal("cannot allocate the guest's environment");
	for (index = 0; index < argc && count < ARGUMENT_MAXIMUM; index++)
	{
		size_t length = strlen(argv[index]) + 1;

		/* (the Finder's process serial number) */
		if (index && !strncmp(argv[index], "-psn_", 5))
			continue;
		if (strings + length > memory + size)
			break;
		memcpy(strings, index ? argv[index] : "halo", index ? length : sizeof("halo"));
		guest_argv[count++] = GUEST_ADDRESS(strings);
		strings += index ? length : sizeof("halo");
	}
	guest_argv[count] = 0;
	for (index = 0; index < environment->count; index++)
	{
		size_t length = strlen(environment->entries[index]) + 1;

		if (strings + length > memory + size)
			break;
		memcpy(strings, environment->entries[index], length);
		environ_list[index] = GUEST_ADDRESS(strings);
		strings += length;
	}
	environ_list[index] = 0;
	boot->argc = (uint32_t)count;
	boot->argv = GUEST_ADDRESS(guest_argv);
	boot->environment = GUEST_ADDRESS(environ_list);
	/* (the guest's page: its musl's and the Xbox's, 4 KB; host_memory.c
	maps them on macOS's 16 KB ones) */
	boot->page_size = 0x1000;
	return GUEST_ADDRESS(boot);
}

/* ---------- main */

int main(int argc, char *argv[])
{
	extern char **environ;
	struct environment environment = { { 0 }, 0 };
	char path[PATH_MAX], zone[64];
	size_t image_size = 0;
	void *image;
	char **variable;

	find_app_folders();
	find_data_root();
	open_log();
	host_logf(HOST_LOG_INFO, "Halo for macOS starting; data in %s", data_root);
	if (chdir(data_root) != 0)
		host_logf(HOST_LOG_WARN, "cannot enter %s: %s", data_root, strerror(errno));
	/* a write to a connection the other end closed fails instead of ending
	the game (as sdl_platform.c asks on Linux, where the guest's signal()
	reaches the system) */
	signal(SIGPIPE, SIG_IGN);
	host_install_signal_handlers();

	if (host_memory_reserve() != 0)
		host_fatal("cannot reserve memory for the game");
	snprintf(path, sizeof(path), "%s/halo_guest.elf", resources);
	image = read_file(path, &image_size);
	if (!image)
		host_fatal("cannot read the game image %s: %s", path, strerror(errno));
	if (host_load_image(image, image_size) != 0)
		host_fatal("cannot load the game image; see the log for details");
	free(image);
	if (getenv("HALO_MEMORY_WATCH") && !strcmp(getenv("HALO_MEMORY_WATCH"), "0"))
	{
		host_memory_watch_use_hashes();
		host_logf(HOST_LOG_INFO, "write tracking: page hashes (HALO_MEMORY_WATCH=0)");
	}
	else
	{
		host_logf(HOST_LOG_INFO, "write tracking: page protection");
	}
	write_brokers();

	/* (no window restored, nor asked about after a crash: host_cocoa.m) */
	host_cocoa_no_window_restoration();
	/* (window managers' questions answered as they come, not a frame each:
	host_cocoa.m) */
	host_cocoa_accessibility_watch();
	/* OpenGL ES through the app's ANGLE (SDL's EGL), on Metal rather than on
	ANGLE's other backend, macOS's OpenGL (which Apple no longer develops) */
	setenv("ANGLE_DEFAULT_PLATFORM", "metal", 0);
	SDL_SetHint(SDL_HINT_EGL_LIBRARY, host_library_path("libEGL.dylib"));
	SDL_SetHint(SDL_HINT_OPENGL_LIBRARY, host_library_path("libGLESv2.dylib"));

	/* the user's settings for one run (HALO_*, port/linux/README.md) and
	SDL's, then the folders */
	for (variable = environ; *variable; variable++)
	{
		char *equals = strchr(*variable, '=');

		if (equals && (!strncmp(*variable, "HALO_", 5) || !strncmp(*variable, "SDL_", 4) ||
			!strncmp(*variable, "LANG=", 5) || !strncmp(*variable, "LC_", 3)))
		{
			*equals = 0;
			environment_set(&environment, *variable, equals + 1);
			*equals = '=';
		}
	}
	environment_set(&environment, "HOME", getenv("HOME") ? getenv("HOME") : data_root);
	snprintf(path, sizeof(path), "%s/save", data_root);
	if (!getenv("HALO_SAVE_ROOT"))
		environment_set(&environment, "HALO_SAVE_ROOT", path);
	time_zone(zone, sizeof(zone));
	environment_set(&environment, "TZ", zone);
	host_run_guest_main(make_boot(argc, argv, &environment));
}
