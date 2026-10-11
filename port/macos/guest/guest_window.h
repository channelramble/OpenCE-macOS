/*
GUEST_WINDOW.H

The macOS guest's memory as the host addresses it (port/macos/README.md).
The guest's addresses are 32-bit, as the Android guest's are, but macOS
gives an arm64 process nothing below 4 GB: the host reserves a 4 GB window
at a 4 GB-aligned base instead, and guest address A is host address
base + A. While guest code runs, x28 holds the base (tools/macos_sfi.py).

A pointer handed to a host function as a host pointer is widened here (the
generated OpenGL ES and posix_* wrappers; the host_* imports' stubs widen
their own, tools/macos_imports.py). NULL stays NULL.
*/

#ifndef __GUEST_WINDOW_H
#define __GUEST_WINDOW_H

static inline unsigned long long guest_window_base(void)
{
	unsigned long long base;

	/* (the one read of x28 the rewrite allows) */
	__asm__("mov %0, x28" : "=r"(base));
	return base;
}

static inline unsigned long long guest_window_pointer(const void *pointer)
{
	return pointer ? guest_window_base() + (unsigned long long)(unsigned int)pointer : 0;
}

#endif
