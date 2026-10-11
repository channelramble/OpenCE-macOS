/*
HOST.H

Internals of the macOS port's host (port/macos/README.md): the arm64
process that reserves the guest's 4 GB window, loads the guest image into it
and serves the guest's requests over SDL3 and ANGLE.

The guest's addresses are 32-bit, as on Android, but macOS keeps the first
4 GB of an arm64 process unmapped: guest address A is host address
host_window_base + A instead, with the base 4 GB-aligned. The guest's code
addresses the window through x28 (tools/macos_sfi.py). A host function the
guest imports gets its pointer parameters as host pointers already (the
import stubs widen them, tools/macos_imports.py); a parameter declared as an
unsigned int is a guest address, which HOST_POINTER widens.
*/

#ifndef __HALO_MACOS_HOST_H
#define __HALO_MACOS_HOST_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "halo_android_abi.h"

/* ---------- the window */

#define HOST_WINDOW_SIZE 0x100000000ULL

extern uint64_t host_window_base;

/* the host address of a guest address (NULL for 0) */
#define HOST_POINTER(type, guest) \
	((type)(uintptr_t)((uint32_t)(guest) ? host_window_base + (uint32_t)(guest) : 0))
/* the guest address of a host pointer into the window */
#define GUEST_ADDRESS(pointer) ((uint32_t)((uintptr_t)(pointer) - host_window_base))

/* ---------- logging and termination (host_main.c) */

void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
#define HOST_LOG_INFO 4
#define HOST_LOG_WARN 5
#define HOST_LOG_ERROR 6
void host_exit(int code) __attribute__((noreturn));
/* the host's errno on this thread, as the guest's (Linux) number */
int host_errno(void);
/* logs, shows the message to the player and terminates */
void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));
/* the data folder (config.toml, maps/, saves), without a trailing slash */
const char *host_data_root(void);
/* AppKit's restoration of the windows, off, and its question after a crash
(host_cocoa.m): before SDL starts AppKit */
void host_cocoa_no_window_restoration(void);
/* Accessibility's questions to the app (host_cocoa.m): counted from before
SDL starts AppKit, and answered as they come after a pump of the events
that answered one (on the main thread; nothing elsewhere) */
void host_cocoa_accessibility_watch(void);
void host_cocoa_accessibility_answer(void);

/* ---------- guest memory (host_memory.c)

All the guest's memory is in the window. The host reserves it at start-up and
hands out pages for everything the guest maps without asking for an address
(its malloc arenas, thread stacks) from pools inside it; the fixed ranges
(the Xbox window, the Custom Edition tag cache, the image) are the guest's to
map where it wants them. */

/* reserves the window; 0 on success */
int host_memory_reserve(void);
/* maps the image's code (and read-only data) as executable memory, writable
by this thread until host_memory_seal_code, and its data read-write; host
addresses of the guest range given; 0 on success */
int host_memory_map_image(uint32_t code_start, uint32_t code_end, uint32_t data_start, uint32_t data_end);
/* the image's code becomes executable for every thread */
void host_memory_seal_code(void);
/* page-granular allocations in the window's pools (host pointers); NULL on
failure */
void *host_low_map(size_t size, int protection);
void host_low_unmap(void *address, size_t size);
/* the guest's mmap, munmap, mprotect and madvise (Linux flags and
protections; results as the system call's, -errno on failure) */
long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset);
long host_guest_munmap(uint64_t address, uint64_t size);
long host_guest_mprotect(uint64_t address, uint64_t size, int protection);
long host_guest_madvise(uint64_t address, uint64_t size, int advice);
/* compares page contents for the write tracking instead of catching page
faults (host_watch_hash.h); before the guest starts */
void host_memory_watch_use_hashes(void);
void host_install_signal_handlers(void);

/* ---------- the guest image (host_loader.c) */

struct host_guest_image
{
	const struct halo_guest_header *header;
	uint32_t base, end;
};

extern struct host_guest_image host_image;

/* maps the image from the ELF file in memory; returns 0 on success */
int host_load_image(const void *elf, size_t size);

/* ---------- entering guest code (host_thread.c, host_entry.S) */

/* calls the guest function at guest address function with up to four
32-bit arguments on this thread, which must have been made by
host_native_thread_create (giving the thread a guest struct pthread first if
it has none); returns the guest's w0 */
uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
/* starts a thread running function(argument) with its stack in the window,
so that it can call guest code; the stack is freed after it exits. Returns
0 or an errno value */
int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size);
/* runs the guest's __guest_start on this thread (the process's main thread,
which SDL's windows and events need) on a stack in the window; does not
return */
void host_run_guest_main(uint32_t boot) __attribute__((noreturn));

/* host_entry.S: function (a host address) with arguments a to d and x28 the
window's base; its x0 */
uint64_t host_enter_guest(uint64_t function, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t base);
/* the same on a new stack, whose top is stack_top; never returns */
void host_enter_guest_on_stack(uint64_t function, uint64_t argument, uint64_t stack_top, uint64_t base)
	__attribute__((noreturn));

/* ---------- imports (generated host_import_table.c; host_gl.c) */

void *host_resolve_import(const char *name);
void *host_gl_resolve(const char *name);

/* ---------- system calls (host_syscall.c) */

/* the Linux errno number of a host one */
int host_linux_errno(int error);
/* the guest's system call layer's state for a host thread that ends */
void host_syscall_thread_exited(void);

#endif
