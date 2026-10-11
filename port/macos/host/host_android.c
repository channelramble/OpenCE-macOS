/*
HOST_ANDROID.C

The Android app's host services (port/android/host_imports.list), which the
macOS guest imports with the rest of that list but never calls: the platform
layer calls them only in the Android build (HALO_ANDROID). They answer as
on a device without them.
*/

#include "host.h"

#include <string.h>

void host_android_path(int which, char *buffer, uint32_t size)
{
	(void)which;
	if (size)
		buffer[0] = 0;
}

void host_gesture_insets(int *insets)
{
	memset(insets, 0, 4 * sizeof(int));
}

void host_touch_read(int *state)
{
	memset(state, 0, 7 * sizeof(int));
}

void host_touch_look_read(float *delta)
{
	memset(delta, 0, 4 * sizeof(float));
}

void host_touch_rumble(uint32_t low, uint32_t high)
{
	(void)low;
	(void)high;
}

void host_touch_scene(int scene)
{
	(void)scene;
}

void host_touch_bindings(const int *controls)
{
	(void)controls;
}
