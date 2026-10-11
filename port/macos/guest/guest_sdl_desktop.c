/*
GUEST_SDL_DESKTOP.C

The SDL3 functions the platform layer's desktop code calls
(port/linux/src/sdl_platform.c, menu_files.c, port_config.c) beyond those
port/android/guest/runtime/guest_sdl.c bridges, for the macOS guest. SDL
runs in the host (port/macos/host/host_sdl.c); as in guest_sdl.c, its
objects are small integer handles here, and what it returns in memory of
its own (display modes, device lists, names) is copied into the guest's.

SDL calls a file dialog's callback on a thread of its own; here the guest
polls for the answer whenever it pumps the events, and calls the callback
there, on its own thread.
*/

/* before SDL: musl's alloca.h must come first */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>

#include "guest_host_macos.h"

/* the bytes of a display mode both ABIs lay out alike: all but its
last member, the host's pointer */
#define DISPLAY_MODE_SHARED_BYTES offsetof(SDL_DisplayMode, internal)

/* ---------- atomics (no host needed) */

int SDL_GetAtomicInt(SDL_AtomicInt *atomic)
{
	return __atomic_load_n(&atomic->value, __ATOMIC_SEQ_CST);
}

int SDL_SetAtomicInt(SDL_AtomicInt *atomic, int value)
{
	return __atomic_exchange_n(&atomic->value, value, __ATOMIC_SEQ_CST);
}

int SDL_AddAtomicInt(SDL_AtomicInt *atomic, int value)
{
	return __atomic_fetch_add(&atomic->value, value, __ATOMIC_SEQ_CST);
}

/* ---------- time and hints */

Uint64 SDL_GetTicksNS(void)
{
	return (Uint64)host_sdl_ticks_ns();
}

void SDL_DelayPrecise(Uint64 nanoseconds)
{
	host_sdl_delay_precise((long long)nanoseconds);
}

bool SDL_GetHintBoolean(const char *name, bool default_value)
{
	return host_sdl_get_hint_boolean(name, default_value) != 0;
}

/* ---------- windows */

void SDL_DestroyWindow(SDL_Window *window)
{
	host_sdl_destroy_window((unsigned int)window);
}

SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *window)
{
	return (SDL_WindowFlags)host_sdl_window_flags((unsigned int)window);
}

bool SDL_GetWindowSize(SDL_Window *window, int *width, int *height)
{
	int w = 0, h = 0;

	host_sdl_window_size((unsigned int)window, &w, &h);
	if (width)
		*width = w;
	if (height)
		*height = h;
	return true;
}

bool SDL_SetWindowSize(SDL_Window *window, int width, int height)
{
	return host_sdl_set_window_size((unsigned int)window, width, height) != 0;
}

bool SDL_SetWindowFullscreen(SDL_Window *window, bool fullscreen)
{
	return host_sdl_set_window_fullscreen((unsigned int)window, fullscreen) != 0;
}

bool SDL_SetWindowFullscreenMode(SDL_Window *window, const SDL_DisplayMode *mode)
{
	return host_sdl_set_window_fullscreen_mode((unsigned int)window, mode) != 0;
}

void SDL_WarpMouseInWindow(SDL_Window *window, float x, float y)
{
	host_sdl_warp_mouse((unsigned int)window, x, y);
}

/* ---------- displays */

SDL_DisplayID SDL_GetPrimaryDisplay(void)
{
	return (SDL_DisplayID)host_sdl_primary_display();
}

SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *window)
{
	return (SDL_DisplayID)host_sdl_display_for_window((unsigned int)window);
}

bool SDL_GetDisplayUsableBounds(SDL_DisplayID display, SDL_Rect *rectangle)
{
	return host_sdl_display_usable_bounds(display, &rectangle->x) != 0;
}

const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID display)
{
	static SDL_DisplayMode mode;

	memset(&mode, 0, sizeof(mode));
	return host_sdl_desktop_display_mode(display, &mode) ? &mode : NULL;
}

const SDL_DisplayMode *SDL_GetCurrentDisplayMode(SDL_DisplayID display)
{
	static SDL_DisplayMode mode;

	memset(&mode, 0, sizeof(mode));
	return host_sdl_current_display_mode(display, &mode) ? &mode : NULL;
}

bool SDL_GetClosestFullscreenDisplayMode(SDL_DisplayID display, int width, int height, float refresh_rate,
	bool include_high_density_modes, SDL_DisplayMode *closest)
{
	memset(closest, 0, sizeof(*closest));
	return host_sdl_closest_display_mode(display, width, height, refresh_rate, include_high_density_modes,
		closest) != 0;
}

SDL_DisplayMode **SDL_GetFullscreenDisplayModes(SDL_DisplayID display, int *count)
{
	enum { CAPACITY = 128 };
	unsigned char *shared = malloc(CAPACITY * DISPLAY_MODE_SHARED_BYTES);
	SDL_DisplayMode **list;
	SDL_DisplayMode *modes;
	int found, index;

	if (count)
		*count = 0;
	if (!shared)
		return NULL;
	found = host_sdl_fullscreen_display_modes(display, shared, CAPACITY);
	/* (one allocation, as SDL's: the pointers, a NULL, then the modes) */
	list = malloc((found + 1) * sizeof(SDL_DisplayMode *) + found * sizeof(SDL_DisplayMode));
	if (!list)
	{
		free(shared);
		return NULL;
	}
	modes = (SDL_DisplayMode *)(list + found + 1);
	for (index = 0; index < found; index++)
	{
		memset(&modes[index], 0, sizeof(modes[index]));
		memcpy(&modes[index], shared + index * DISPLAY_MODE_SHARED_BYTES, DISPLAY_MODE_SHARED_BYTES);
		list[index] = &modes[index];
	}
	list[found] = NULL;
	free(shared);
	if (count)
		*count = found;
	return list;
}

/* ---------- audio devices */

static SDL_AudioDeviceID *audio_devices(int recording, int *count)
{
	unsigned int ids[64];
	int found = host_sdl_audio_devices(recording, ids, 64), index;
	SDL_AudioDeviceID *result = malloc((found + 1) * sizeof(SDL_AudioDeviceID));

	if (count)
		*count = 0;
	if (!result)
		return NULL;
	for (index = 0; index < found; index++)
		result[index] = ids[index];
	result[found] = 0;
	if (count)
		*count = found;
	return result;
}

SDL_AudioDeviceID *SDL_GetAudioPlaybackDevices(int *count)
{
	return audio_devices(0, count);
}

SDL_AudioDeviceID *SDL_GetAudioRecordingDevices(int *count)
{
	return audio_devices(1, count);
}

const char *SDL_GetAudioDeviceName(SDL_AudioDeviceID device)
{
	/* (SDL's stay valid; these last for the next few calls, which is as
	long as the platform layer keeps one) */
	static char names[8][256];
	static int next;
	char *name = names[next++ & 7];

	return host_sdl_audio_device_name(device, name, sizeof(names[0])) ? name : NULL;
}

/* ---------- events and text input */

static void dialogs_poll(void);

void SDL_PumpEvents(void)
{
	host_sdl_pump_events();
	dialogs_poll();
}

bool SDL_PushEvent(SDL_Event *event)
{
	return host_sdl_push_event(event) != 0;
}

SDL_PropertiesID SDL_CreateProperties(void)
{
	return (SDL_PropertiesID)host_sdl_create_properties();
}

void SDL_DestroyProperties(SDL_PropertiesID properties)
{
	host_sdl_destroy_properties(properties);
}

bool SDL_SetBooleanProperty(SDL_PropertiesID properties, const char *name, bool value)
{
	return host_sdl_set_boolean_property(properties, name, value) != 0;
}

bool SDL_SetNumberProperty(SDL_PropertiesID properties, const char *name, Sint64 value)
{
	return host_sdl_set_number_property(properties, name, (long long)value) != 0;
}

bool SDL_StartTextInputWithProperties(SDL_Window *window, SDL_PropertiesID properties)
{
	return host_sdl_start_text_input((unsigned int)window, properties) != 0;
}

bool SDL_StopTextInput(SDL_Window *window)
{
	return host_sdl_stop_text_input((unsigned int)window) != 0;
}

bool SDL_TextInputActive(SDL_Window *window)
{
	return host_sdl_text_input_active((unsigned int)window) != 0;
}

bool SDL_HasScreenKeyboardSupport(void)
{
	return host_sdl_has_screen_keyboard() != 0;
}

/* ---------- message boxes */

bool SDL_ShowMessageBox(const SDL_MessageBoxData *data, int *buttonid)
{
	enum { BUTTONS = 8, TEXTS = 512 };
	int flags[BUTTONS], ids[BUTTONS], pushed = -1, count = 0, index;
	char texts[TEXTS];
	size_t used = 0;
	bool shown;

	for (index = 0; index < data->numbuttons && count < BUTTONS; index++)
	{
		const char *text = data->buttons[index].text ? data->buttons[index].text : "";
		size_t length = strlen(text) + 1;

		if (used + length > sizeof(texts))
			break;
		memcpy(texts + used, text, length);
		used += length;
		flags[count] = (int)data->buttons[index].flags;
		ids[count] = data->buttons[index].buttonID;
		count++;
	}
	shown = host_sdl_show_message_box((unsigned int)data->flags, data->title ? data->title : "",
		data->message ? data->message : "", count, flags, ids, texts, &pushed) != 0;
	if (buttonid)
		*buttonid = pushed;
	return shown;
}

/* ---------- file dialogs */

#define DIALOGS 4

static struct
{
	unsigned int request;
	SDL_DialogFileCallback callback;
	void *userdata;
} dialogs[DIALOGS];

/* the filters' names and patterns, each ended by a NUL, into the buffers;
0 if they do not fit */
static int filters_pack(const SDL_DialogFileFilter *filters, int count, char *names, char *patterns, size_t size)
{
	size_t name_used = 0, pattern_used = 0;
	int index;

	for (index = 0; index < count; index++)
	{
		const char *name = filters[index].name ? filters[index].name : "";
		const char *pattern = filters[index].pattern ? filters[index].pattern : "";

		if (name_used + strlen(name) + 1 > size || pattern_used + strlen(pattern) + 1 > size)
			return 0;
		strcpy(names + name_used, name);
		strcpy(patterns + pattern_used, pattern);
		name_used += strlen(name) + 1;
		pattern_used += strlen(pattern) + 1;
	}
	return 1;
}

void SDL_ShowOpenFileDialog(SDL_DialogFileCallback callback, void *userdata, SDL_Window *window,
	const SDL_DialogFileFilter *filters, int nfilters, const char *default_location, bool allow_many)
{
	char names[512], patterns[512];
	unsigned int request = 0;
	int slot;

	if (nfilters < 0 || !filters)
		nfilters = 0;
	for (slot = 0; slot < DIALOGS && dialogs[slot].request; slot++)
	{
	}
	if (slot < DIALOGS && filters_pack(filters, nfilters, names, patterns, sizeof(names)))
	{
		request = host_sdl_show_open_file_dialog((unsigned int)window, names, patterns, nfilters, default_location,
			allow_many);
	}
	if (!request)
	{
		/* (SDL's own failure: no list at all) */
		if (callback)
			callback(userdata, NULL, -1);
		return;
	}
	dialogs[slot].request = request;
	dialogs[slot].callback = callback;
	dialogs[slot].userdata = userdata;
	/* (the picker may have answered at once) */
	dialogs_poll();
}

static void dialogs_poll(void)
{
	int slot;

	for (slot = 0; slot < DIALOGS; slot++)
	{
		enum { PATHS = 16 };
		char buffer[4096];
		const char *files[PATHS + 1];
		int filter = -1, state, count = 0;
		SDL_DialogFileCallback callback;
		void *userdata;

		if (!dialogs[slot].request)
			continue;
		state = host_sdl_dialog_result(dialogs[slot].request, buffer, sizeof(buffer), &filter);
		if (!state)
			continue;
		callback = dialogs[slot].callback;
		userdata = dialogs[slot].userdata;
		dialogs[slot].request = 0;
		if (state == 1)
		{
			const char *path = buffer;

			while (*path && count < PATHS)
			{
				files[count++] = path;
				path += strlen(path) + 1;
			}
		}
		files[count] = NULL;
		if (callback)
			callback(userdata, state == 3 ? NULL : files, filter);
	}
}

/* ---------- files */

const char *SDL_GetBasePath(void)
{
	static char path[1024];

	if (!path[0])
		host_sdl_base_path(path, sizeof(path));
	return path;
}

char **SDL_GlobDirectory(const char *path, const char *pattern, SDL_GlobFlags flags, int *count)
{
	char *buffer;
	char **list;
	char *names;
	int found, index;
	size_t size = 1 << 16;

	if (count)
		*count = 0;
	for (;;)
	{
		buffer = malloc(size);
		if (!buffer)
			return NULL;
		found = host_sdl_glob_directory(path, pattern, (unsigned int)flags, buffer, (unsigned int)size);
		if (found >= 0 || size >= (1 << 24))
			break;
		free(buffer);
		size *= 4;
	}
	if (found < 0)
	{
		free(buffer);
		return NULL;
	}
	/* (one allocation, as SDL's: the pointers, a NULL, then the names) */
	{
		size_t names_size = 0;
		const char *name = buffer;

		for (index = 0; index < found; index++)
		{
			names_size += strlen(name) + 1;
			name += strlen(name) + 1;
		}
		list = malloc((found + 1) * sizeof(char *) + names_size);
		if (!list)
		{
			free(buffer);
			return NULL;
		}
		names = (char *)(list + found + 1);
		memcpy(names, buffer, names_size);
	}
	for (index = 0; index < found; index++)
	{
		list[index] = names;
		names += strlen(names) + 1;
	}
	list[found] = NULL;
	free(buffer);
	if (count)
		*count = found;
	return list;
}

/* ---------- renderers */

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, const char *name)
{
	return (SDL_Renderer *)host_sdl_create_renderer((unsigned int)window, name);
}

void SDL_DestroyRenderer(SDL_Renderer *renderer)
{
	host_sdl_destroy_renderer((unsigned int)renderer);
}

bool SDL_SetRenderVSync(SDL_Renderer *renderer, int vsync)
{
	return host_sdl_set_render_vsync((unsigned int)renderer, vsync) != 0;
}

bool SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	return host_sdl_set_render_draw_color((unsigned int)renderer, r, g, b, a) != 0;
}

bool SDL_RenderClear(SDL_Renderer *renderer)
{
	return host_sdl_render_clear((unsigned int)renderer) != 0;
}

bool SDL_SetRenderScale(SDL_Renderer *renderer, float x, float y)
{
	return host_sdl_set_render_scale((unsigned int)renderer, x, y) != 0;
}

bool SDL_RenderDebugText(SDL_Renderer *renderer, float x, float y, const char *text)
{
	return host_sdl_render_debug_text((unsigned int)renderer, x, y, text) != 0;
}

bool SDL_RenderFillRect(SDL_Renderer *renderer, const SDL_FRect *rectangle)
{
	return host_sdl_render_fill_rect((unsigned int)renderer, &rectangle->x) != 0;
}

bool SDL_RenderPresent(SDL_Renderer *renderer)
{
	return host_sdl_render_present((unsigned int)renderer) != 0;
}
