/*
HOST_GL.C

OpenGL ES for the guest, from ANGLE (libGLESv2.dylib in the app, on Metal).
The guest's generated entry points (tools/android_gl_stubs.py --window)
import hostgl_<function>, resolved here to ANGLE's function at load time,
before there is a context (ANGLE's entry points find the current one); the
arguments arrive with host types and host pointers. The helpers below are
the Android host's (port/android/host/host_gl.c), calling through the same
entry points.
*/

#include "host.h"

#define GL_GLES_PROTOTYPES 0
#include <GLES3/gl32.h>
#include <dlfcn.h>
#include <string.h>

/* host_main.c: a library of the app's (its Frameworks folder) */
const char *host_library_path(const char *name);

static void *gles_library(void)
{
	static void *library;

	if (!library)
	{
		library = dlopen(host_library_path("libGLESv2.dylib"), RTLD_NOW | RTLD_GLOBAL);
		if (!library)
			host_logf(HOST_LOG_ERROR, "cannot load ANGLE: %s", dlerror());
	}
	return library;
}

void *host_gl_resolve(const char *name)
{
	void *library = gles_library();

	return library ? dlsym(library, name) : NULL;
}

static struct
{
	PFNGLGETSTRINGPROC GetString;
	PFNGLGETSTRINGIPROC GetStringi;
	PFNGLGETINTEGERVPROC GetIntegerv;
	PFNGLBINDBUFFERPROC BindBuffer;
	PFNGLMAPBUFFERRANGEPROC MapBufferRange;
	PFNGLUNMAPBUFFERPROC UnmapBuffer;
	PFNGLBUFFERSUBDATAPROC BufferSubData;
	PFNGLFENCESYNCPROC FenceSync;
	PFNGLDELETESYNCPROC DeleteSync;
	PFNGLCLIENTWAITSYNCPROC ClientWaitSync;
} gl;

static void gl_load(void)
{
	if (gl.GetString)
		return;
	gl.GetStringi = (PFNGLGETSTRINGIPROC)host_gl_resolve("glGetStringi");
	gl.GetIntegerv = (PFNGLGETINTEGERVPROC)host_gl_resolve("glGetIntegerv");
	gl.BindBuffer = (PFNGLBINDBUFFERPROC)host_gl_resolve("glBindBuffer");
	gl.MapBufferRange = (PFNGLMAPBUFFERRANGEPROC)host_gl_resolve("glMapBufferRange");
	gl.UnmapBuffer = (PFNGLUNMAPBUFFERPROC)host_gl_resolve("glUnmapBuffer");
	gl.BufferSubData = (PFNGLBUFFERSUBDATAPROC)host_gl_resolve("glBufferSubData");
	gl.FenceSync = (PFNGLFENCESYNCPROC)host_gl_resolve("glFenceSync");
	gl.DeleteSync = (PFNGLDELETESYNCPROC)host_gl_resolve("glDeleteSync");
	gl.ClientWaitSync = (PFNGLCLIENTWAITSYNCPROC)host_gl_resolve("glClientWaitSync");
	gl.GetString = (PFNGLGETSTRINGPROC)host_gl_resolve("glGetString");
	if (!gl.GetString)
		host_fatal("OpenGL ES (ANGLE's libGLESv2.dylib) is missing from the app");
}

void host_gl_get_string(uint32_t name, int index, char *buffer, uint32_t size)
{
	const GLubyte *text;

	gl_load();
	text = index >= 0 ? gl.GetStringi(name, (GLuint)index) : gl.GetString(name);
	if (!size)
		return;
	buffer[0] = 0;
	if (text)
	{
		strncpy(buffer, (const char *)text, size - 1);
		buffer[size - 1] = 0;
	}
}

int host_gl_has_extension(const char *name)
{
	GLint count = 0, index;

	gl_load();
	gl.GetIntegerv(GL_NUM_EXTENSIONS, &count);
	for (index = 0; index < count; index++)
	{
		const char *extension = (const char *)gl.GetStringi(GL_EXTENSIONS, (GLuint)index);

		if (extension && !strcmp(extension, name))
			return 1;
	}
	return 0;
}

/* copies size bytes of a buffer object (the snapshots of the visibility
tests' counters, d3d8_gl.c); ES has no glGetBufferSubData */
void host_gl_read_buffer(uint32_t buffer, uint32_t offset, uint32_t size, void *data)
{
	const void *mapping;

	gl_load();
	gl.BindBuffer(GL_COPY_READ_BUFFER, buffer);
	mapping = gl.MapBufferRange(GL_COPY_READ_BUFFER, offset, size, GL_MAP_READ_BIT);
	if (mapping)
	{
		memcpy(data, mapping, size);
		gl.UnmapBuffer(GL_COPY_READ_BUFFER);
	}
	else
	{
		memset(data, 0, size);
	}
	gl.BindBuffer(GL_COPY_READ_BUFFER, 0);
}

/* the frames' fences, as port/android/host/host_gl.c's */
#define FRAME_FENCE_SLOTS 8

static GLsync frame_fences[FRAME_FENCE_SLOTS];

void host_gl_fence_frame(uint32_t slot)
{
	gl_load();
	if (slot >= FRAME_FENCE_SLOTS)
		return;
	if (frame_fences[slot])
		gl.DeleteSync(frame_fences[slot]);
	frame_fences[slot] = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void host_gl_wait_frame(uint32_t slot)
{
	gl_load();
	if (slot >= FRAME_FENCE_SLOTS || !frame_fences[slot])
		return;
	/* at most a second: a lost context must not hang the game */
	gl.ClientWaitSync(frame_fences[slot], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
	gl.DeleteSync(frame_fences[slot]);
	frame_fences[slot] = NULL;
}

/* an unsynchronized write into the buffer bound to target (the renderer
waits for the ring slot first: port/android/host/host_gl.c has why) */
void host_gl_buffer_write(uint32_t target, uint32_t offset, uint32_t size, const void *data)
{
	void *mapping;

	gl_load();
	mapping = gl.MapBufferRange(target, offset, size, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
	if (!mapping)
	{
		gl.BufferSubData(target, offset, size, data);
		return;
	}
	memcpy(mapping, data, size);
	gl.UnmapBuffer(target);
}
