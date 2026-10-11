/*
HOST_SDL.C

SDL3 on behalf of the macOS guest: what port/android/host/host_sdl.c does for
the Android guest (port/android/guest/runtime/guest_sdl.c), from which this
file is taken, and the desktop's windows, displays, dialogs, audio devices
and renderers (port/macos/guest/guest_sdl_desktop.c). SDL objects are 64-bit
pointers, which the guest cannot hold; it gets small handles into the table
here instead.

The guest calls these on its own threads, whose stacks are in the window;
its main thread is the process's, which SDL needs for its windows and events
on macOS (host_thread.c). SDL's audio thread has no guest stack, so the
audio callback is handed to a thread that has one, and a file dialog's
answer is kept for the guest to poll.
*/

#include "host.h"

#include <SDL3/SDL.h>
#include <pthread.h>
#include <string.h>

#define HANDLE_COUNT 256

enum handle_type
{
	_handle_free,
	_handle_window,
	_handle_context,
	_handle_gamepad,
	_handle_audio,
	_handle_renderer,
};

struct handle
{
	int type;
	void *object;
	/* an audio stream with a callback: its binding (below) */
	void *binding;
};

static struct handle handles[HANDLE_COUNT];
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t handle_new(int type, void *object)
{
	uint32_t index;

	if (!object)
		return 0;
	pthread_mutex_lock(&handle_lock);
	/* an object that already has a handle keeps it */
	for (index = 1; index < HANDLE_COUNT; index++)
	{
		if (handles[index].type == type && handles[index].object == object)
		{
			pthread_mutex_unlock(&handle_lock);
			return index;
		}
	}
	for (index = 1; index < HANDLE_COUNT; index++)
	{
		if (handles[index].type == _handle_free)
		{
			handles[index].type = type;
			handles[index].object = object;
			handles[index].binding = NULL;
			pthread_mutex_unlock(&handle_lock);
			return index;
		}
	}
	pthread_mutex_unlock(&handle_lock);
	host_logf(HOST_LOG_ERROR, "out of SDL handles");
	return 0;
}

static void *handle_get(uint32_t handle, int type)
{
	void *object = NULL;

	if (handle == 0 || handle >= HANDLE_COUNT)
		return NULL;
	pthread_mutex_lock(&handle_lock);
	if (handles[handle].type == type)
		object = handles[handle].object;
	pthread_mutex_unlock(&handle_lock);
	return object;
}

/* the object of a handle, which the handle no longer refers to; NULL when
there is none. *binding gets what handle_new's caller stored (audio) */
static void *handle_release(uint32_t handle, int type, void **binding)
{
	void *object = NULL;

	if (binding)
		*binding = NULL;
	if (handle == 0 || handle >= HANDLE_COUNT)
		return NULL;
	pthread_mutex_lock(&handle_lock);
	if (handles[handle].type == type)
	{
		object = handles[handle].object;
		if (binding)
			*binding = handles[handle].binding;
		handles[handle].type = _handle_free;
		handles[handle].object = NULL;
		handles[handle].binding = NULL;
	}
	pthread_mutex_unlock(&handle_lock);
	return object;
}

/* ---------- general */

int host_sdl_init(uint32_t flags)
{
	return SDL_Init((SDL_InitFlags)flags);
}

int host_sdl_set_hint(const char *name, const char *value)
{
	return SDL_SetHint(name, value);
}

void host_sdl_get_error(char *buffer, uint32_t size)
{
	SDL_strlcpy(buffer, SDL_GetError(), size);
}

void host_sdl_scancode_name(int32_t scancode, char *buffer, uint32_t size)
{
	SDL_strlcpy(buffer, SDL_GetScancodeName((SDL_Scancode)scancode), size);
}

int32_t host_sdl_scancode_from_name(const char *name)
{
	return (int32_t)SDL_GetScancodeFromName(name);
}

int64_t host_sdl_ticks(void)
{
	return (int64_t)SDL_GetTicks();
}

int64_t host_sdl_thread_id(void)
{
	return (int64_t)SDL_GetCurrentThreadID();
}

/* ---------- video */

uint32_t host_sdl_create_window(const char *title, int width, int height, int64_t flags)
{
	return handle_new(_handle_window, SDL_CreateWindow(title, width, height, (SDL_WindowFlags)flags));
}

void host_sdl_window_size_in_pixels(uint32_t window, int *width, int *height)
{
	SDL_Window *object = handle_get(window, _handle_window);

	*width = 0;
	*height = 0;
	if (object)
		SDL_GetWindowSizeInPixels(object, width, height);
}

int host_sdl_set_relative_mouse(uint32_t window, int enabled)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_SetWindowRelativeMouseMode(object, enabled != 0) : 0;
}

int host_sdl_gl_set_attribute(int attribute, int value)
{
	return SDL_GL_SetAttribute((SDL_GLAttr)attribute, value);
}

/* host_cocoa.m: the window's layers made opaque, ANGLE's among them */
void host_cocoa_window_opaque(SDL_Window *window);

uint32_t host_sdl_gl_create_context(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);
	SDL_GLContext context = object ? SDL_GL_CreateContext(object) : NULL;

	if (context)
		host_cocoa_window_opaque(object);
	return handle_new(_handle_context, context);
}

int host_sdl_gl_make_current(uint32_t window, uint32_t context)
{
	return SDL_GL_MakeCurrent(handle_get(window, _handle_window), handle_get(context, _handle_context));
}

int host_sdl_gl_set_swap_interval(int interval)
{
	return SDL_GL_SetSwapInterval(interval);
}

int host_sdl_gl_swap_window(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_GL_SwapWindow(object) : 0;
}

/* ---------- events */

int host_sdl_poll_event(void *event)
{
	SDL_Event host_event;

	if (!SDL_PollEvent(&host_event))
	{
		/* (the events pumped: questions that came with them answered on,
		host_cocoa.m) */
		host_cocoa_accessibility_answer();
		return 0;
	}
	/* the layouts agree except for the pointers of text, drop and user
	events, which the guest does not read */
	memcpy(event, &host_event, sizeof(host_event));
	return 1;
}

/* ---------- gamepads */

int host_sdl_get_gamepads(uint32_t *ids, int capacity)
{
	int count = 0, index;
	SDL_JoystickID *list = SDL_GetGamepads(&count);

	if (!list)
		return 0;
	if (count > capacity)
		count = capacity;
	for (index = 0; index < count; index++)
		ids[index] = list[index];
	SDL_free(list);
	return count;
}

uint32_t host_sdl_open_gamepad(uint32_t id)
{
	SDL_Gamepad *gamepad = SDL_OpenGamepad((SDL_JoystickID)id);

	if (gamepad)
		host_logf(HOST_LOG_INFO, "gamepad %u: %s (type %d, %04x:%04x)", (unsigned)id, SDL_GetGamepadName(gamepad),
			(int)SDL_GetGamepadType(gamepad), SDL_GetGamepadVendor(gamepad), SDL_GetGamepadProduct(gamepad));
	return handle_new(_handle_gamepad, gamepad);
}

uint32_t host_sdl_gamepad_from_id(uint32_t id)
{
	return handle_new(_handle_gamepad, SDL_GetGamepadFromID((SDL_JoystickID)id));
}

/* (the event thread, when the controller goes: sdl_platform.c) */
void host_sdl_close_gamepad(uint32_t gamepad)
{
	SDL_Gamepad *object = handle_release(gamepad, _handle_gamepad, NULL);

	if (object)
		SDL_CloseGamepad(object);
}

int host_sdl_gamepad_axis(uint32_t gamepad, int axis)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_GetGamepadAxis(object, (SDL_GamepadAxis)axis) : 0;
}

int host_sdl_gamepad_button(uint32_t gamepad, int button)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_GetGamepadButton(object, (SDL_GamepadButton)button) : 0;
}

int host_sdl_gamepad_type(uint32_t gamepad)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_GetGamepadType(object) : SDL_GAMEPAD_TYPE_UNKNOWN;
}

int host_sdl_rumble_gamepad(uint32_t gamepad, uint32_t low, uint32_t high, uint32_t milliseconds)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_RumbleGamepad(object, (Uint16)low, (Uint16)high, milliseconds) : 0;
}

/* ---------- audio */

/* SDL calls audio_callback on its own audio thread, which cannot run guest
code; it passes each request to the stream's thread (audio_thread), which
can, and waits for it to be done. SDL holds the stream's lock throughout, so
the audio the guest puts into the stream meanwhile (from audio_thread) is
kept in the binding's buffer instead, and put in by audio_callback once the
guest is done: audio_thread putting it in itself would wait for the lock
forever */
struct audio_binding
{
	uint32_t handle;
	uint32_t callback;
	uint32_t userdata;
	pthread_mutex_t lock;
	pthread_cond_t requested;
	pthread_cond_t done;
	int pending;
	/* the stream is destroyed: the thread frees the binding and ends */
	int quit;
	int additional;
	int total;
	unsigned char *buffer;
	int buffer_length;
	int buffer_size;
};

/* the binding whose callback this thread is running, if any */
static __thread struct audio_binding *calling_back;

static void *audio_thread(void *context)
{
	struct audio_binding *binding = context;

	pthread_mutex_lock(&binding->lock);
	for (;;)
	{
		int additional, total;

		while (!binding->pending && !binding->quit)
			pthread_cond_wait(&binding->requested, &binding->lock);
		if (binding->quit)
			break;
		additional = binding->additional;
		total = binding->total;
		pthread_mutex_unlock(&binding->lock);
		calling_back = binding;
		host_call_guest(binding->callback, binding->userdata, binding->handle, (uint32_t)additional, (uint32_t)total);
		calling_back = NULL;
		pthread_mutex_lock(&binding->lock);
		binding->pending = 0;
		pthread_cond_signal(&binding->done);
	}
	pthread_mutex_unlock(&binding->lock);
	pthread_cond_destroy(&binding->done);
	pthread_cond_destroy(&binding->requested);
	pthread_mutex_destroy(&binding->lock);
	SDL_free(binding->buffer);
	SDL_free(binding);
	return NULL;
}

static void SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
	struct audio_binding *binding = userdata;

	pthread_mutex_lock(&binding->lock);
	binding->additional = additional;
	binding->total = total;
	binding->pending = 1;
	pthread_cond_signal(&binding->requested);
	while (binding->pending)
		pthread_cond_wait(&binding->done, &binding->lock);
	pthread_mutex_unlock(&binding->lock);
	if (binding->buffer_length)
	{
		SDL_PutAudioStreamData(stream, binding->buffer, binding->buffer_length);
		binding->buffer_length = 0;
	}
}

/* audio the guest puts into its stream during the stream's callback, for
audio_callback to put in; 1 on success */
static int audio_keep(struct audio_binding *binding, const void *data, int length)
{
	if (length < 0)
		return 0;
	if (binding->buffer_length + length > binding->buffer_size)
	{
		int size = (binding->buffer_length + length) * 2;
		unsigned char *buffer = SDL_realloc(binding->buffer, (size_t)size);

		if (!buffer)
			return 0;
		binding->buffer = buffer;
		binding->buffer_size = size;
	}
	memcpy(binding->buffer + binding->buffer_length, data, (size_t)length);
	binding->buffer_length += length;
	return 1;
}

uint32_t host_sdl_open_audio_stream(uint32_t device, const void *spec, uint32_t callback, uint32_t userdata)
{
	struct audio_binding *binding;
	SDL_AudioStream *stream;

	/* (one without a callback, the microphone's, which the guest reads:
	nothing to bind) */
	if (!callback)
	{
		stream = SDL_OpenAudioDeviceStream((SDL_AudioDeviceID)device, spec, NULL, NULL);
		return stream ? handle_new(_handle_audio, stream) : 0;
	}
	binding = SDL_calloc(1, sizeof(*binding));
	if (!binding)
		return 0;
	binding->callback = callback;
	binding->userdata = userdata;
	pthread_mutex_init(&binding->lock, NULL);
	pthread_cond_init(&binding->requested, NULL);
	pthread_cond_init(&binding->done, NULL);
	stream = SDL_OpenAudioDeviceStream((SDL_AudioDeviceID)device, spec,
		callback ? audio_callback : NULL, binding);
	if (!stream)
	{
		SDL_free(binding);
		return 0;
	}
	/* the device starts paused, so no callback can run before this */
	binding->handle = handle_new(_handle_audio, stream);
	if (!binding->handle)
	{
		SDL_DestroyAudioStream(stream);
		pthread_cond_destroy(&binding->done);
		pthread_cond_destroy(&binding->requested);
		pthread_mutex_destroy(&binding->lock);
		SDL_free(binding);
		return 0;
	}
	pthread_mutex_lock(&handle_lock);
	handles[binding->handle].binding = binding;
	pthread_mutex_unlock(&handle_lock);
	if (host_native_thread_create(audio_thread, binding, 256 * 1024) != 0)
		host_fatal("cannot start the audio thread");
	return binding->handle;
}

int host_sdl_put_audio_stream_data(uint32_t stream, const void *data, int length)
{
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	if (!object)
		return 0;
	if (calling_back && calling_back->handle == stream)
		return audio_keep(calling_back, data, length);
	return SDL_PutAudioStreamData(object, data, length);
}

int host_sdl_resume_audio_stream_device(uint32_t stream)
{
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	return object ? SDL_ResumeAudioStreamDevice(object) : 0;
}

/* (voice chat's microphone: port/linux/src/voice_audio.c) */
int host_sdl_get_audio_stream_data(uint32_t stream, void *data, int length)
{
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	return object ? SDL_GetAudioStreamData(object, data, length) : -1;
}

int host_sdl_get_audio_stream_available(uint32_t stream)
{
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	return object ? SDL_GetAudioStreamAvailable(object) : -1;
}

/* (a stream with a callback has a thread the guest's callbacks run on:
once no callback can run any more, it is told to end, and frees the
binding and its stack as it goes) */
void host_sdl_destroy_audio_stream(uint32_t stream)
{
	void *context;
	SDL_AudioStream *object = handle_release(stream, _handle_audio, &context);
	struct audio_binding *binding = context;

	if (!object)
		return;
	/* (returns once a callback that is running has finished) */
	SDL_DestroyAudioStream(object);
	if (binding)
	{
		pthread_mutex_lock(&binding->lock);
		binding->quit = 1;
		pthread_cond_signal(&binding->requested);
		pthread_mutex_unlock(&binding->lock);
	}
}

/* ---------- the clipboard (internet play's invite links) */

int host_sdl_set_clipboard_text(const char *text)
{
	return SDL_SetClipboardText(text) ? 1 : 0;
}

void host_sdl_get_clipboard_text(char *buffer, uint32_t size)
{
	char *text = SDL_GetClipboardText();

	SDL_strlcpy(buffer, text ? text : "", size);
	SDL_free(text);
}

/* (Android's notices: none on the desktop) */
int host_sdl_show_toast(const char *message, int duration, int gravity, int x, int y)
{
	(void)message;
	(void)duration;
	(void)gravity;
	(void)x;
	(void)y;
	return 0;
}

/* ---------- a message for the player (a host of another network version) */

int host_sdl_show_simple_message_box(uint32_t flags, const char *title, const char *message)
{
	return SDL_ShowSimpleMessageBox((SDL_MessageBoxFlags)flags, title, message, NULL) ? 1 : 0;
}

/* ---------- the desktop: time and hints (port/macos/guest/guest_sdl_desktop.c) */

int64_t host_sdl_ticks_ns(void)
{
	return (int64_t)SDL_GetTicksNS();
}

void host_sdl_delay_precise(int64_t nanoseconds)
{
	if (nanoseconds > 0)
		SDL_DelayPrecise((Uint64)nanoseconds);
}

int host_sdl_get_hint_boolean(const char *name, int default_value)
{
	return SDL_GetHintBoolean(name, default_value != 0) ? 1 : 0;
}

/* ---------- windows */

void host_sdl_destroy_window(uint32_t window)
{
	SDL_Window *object = handle_release(window, _handle_window, NULL);

	if (object)
		SDL_DestroyWindow(object);
}

int64_t host_sdl_window_flags(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? (int64_t)SDL_GetWindowFlags(object) : 0;
}

void host_sdl_window_size(uint32_t window, int *width, int *height)
{
	SDL_Window *object = handle_get(window, _handle_window);

	*width = 0;
	*height = 0;
	if (object)
		SDL_GetWindowSize(object, width, height);
}

int host_sdl_set_window_size(uint32_t window, int width, int height)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_SetWindowSize(object, width, height) : 0;
}

int host_sdl_set_window_fullscreen(uint32_t window, int fullscreen)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_SetWindowFullscreen(object, fullscreen != 0) : 0;
}

/* the bytes of a display mode both ABIs lay out alike: all but the
pointer it ends with, which SDL finds again from the mode's other members */
#define DISPLAY_MODE_SHARED_BYTES offsetof(SDL_DisplayMode, internal)

int host_sdl_set_window_fullscreen_mode(uint32_t window, const void *mode)
{
	SDL_Window *object = handle_get(window, _handle_window);
	SDL_DisplayMode host_mode;

	if (!object)
		return 0;
	if (!mode)
		return SDL_SetWindowFullscreenMode(object, NULL);
	memset(&host_mode, 0, sizeof(host_mode));
	memcpy(&host_mode, mode, DISPLAY_MODE_SHARED_BYTES);
	return SDL_SetWindowFullscreenMode(object, &host_mode);
}

void host_sdl_warp_mouse(uint32_t window, float x, float y)
{
	SDL_Window *object = handle_get(window, _handle_window);

	if (object)
		SDL_WarpMouseInWindow(object, x, y);
}

/* ---------- displays */

uint32_t host_sdl_primary_display(void)
{
	return SDL_GetPrimaryDisplay();
}

uint32_t host_sdl_display_for_window(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_GetDisplayForWindow(object) : 0;
}

int host_sdl_display_usable_bounds(uint32_t display, int *rectangle)
{
	SDL_Rect bounds;

	if (!SDL_GetDisplayUsableBounds(display, &bounds))
		return 0;
	rectangle[0] = bounds.x;
	rectangle[1] = bounds.y;
	rectangle[2] = bounds.w;
	rectangle[3] = bounds.h;
	return 1;
}

static int mode_out(const SDL_DisplayMode *mode, void *result)
{
	if (!mode)
		return 0;
	memcpy(result, mode, DISPLAY_MODE_SHARED_BYTES);
	return 1;
}

int host_sdl_desktop_display_mode(uint32_t display, void *mode)
{
	return mode_out(SDL_GetDesktopDisplayMode(display), mode);
}

int host_sdl_current_display_mode(uint32_t display, void *mode)
{
	return mode_out(SDL_GetCurrentDisplayMode(display), mode);
}

int host_sdl_closest_display_mode(uint32_t display, int width, int height, float refresh_rate,
	int include_high_density, void *mode)
{
	SDL_DisplayMode closest;

	if (!SDL_GetClosestFullscreenDisplayMode(display, width, height, refresh_rate, include_high_density != 0, &closest))
		return 0;
	return mode_out(&closest, mode);
}

int host_sdl_fullscreen_display_modes(uint32_t display, void *modes, int capacity)
{
	int count = 0, index;
	SDL_DisplayMode **list = SDL_GetFullscreenDisplayModes(display, &count);

	if (!list)
		return 0;
	if (count > capacity)
		count = capacity;
	for (index = 0; index < count; index++)
		memcpy((char *)modes + index * DISPLAY_MODE_SHARED_BYTES, list[index], DISPLAY_MODE_SHARED_BYTES);
	SDL_free(list);
	return count;
}

/* ---------- audio devices */

int host_sdl_audio_devices(int recording, uint32_t *ids, int capacity)
{
	int count = 0, index;
	SDL_AudioDeviceID *list = recording ? SDL_GetAudioRecordingDevices(&count) : SDL_GetAudioPlaybackDevices(&count);

	if (!list)
		return 0;
	if (count > capacity)
		count = capacity;
	for (index = 0; index < count; index++)
		ids[index] = list[index];
	SDL_free(list);
	return count;
}

int host_sdl_audio_device_name(uint32_t device, char *buffer, uint32_t size)
{
	const char *name = SDL_GetAudioDeviceName((SDL_AudioDeviceID)device);

	if (!name || !size)
		return 0;
	SDL_strlcpy(buffer, name, size);
	return 1;
}

/* ---------- events and text input */

void host_sdl_pump_events(void)
{
	SDL_PumpEvents();
	host_cocoa_accessibility_answer();
}

/* (an event without pointers, as the guest makes: the layouts agree) */
int host_sdl_push_event(const void *event)
{
	SDL_Event host_event;

	memcpy(&host_event, event, sizeof(host_event));
	return SDL_PushEvent(&host_event) ? 1 : 0;
}

uint32_t host_sdl_create_properties(void)
{
	return SDL_CreateProperties();
}

void host_sdl_destroy_properties(uint32_t properties)
{
	SDL_DestroyProperties(properties);
}

int host_sdl_set_boolean_property(uint32_t properties, const char *name, int value)
{
	return SDL_SetBooleanProperty(properties, name, value != 0) ? 1 : 0;
}

int host_sdl_set_number_property(uint32_t properties, const char *name, int64_t value)
{
	return SDL_SetNumberProperty(properties, name, value) ? 1 : 0;
}

int host_sdl_start_text_input(uint32_t window, uint32_t properties)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_StartTextInputWithProperties(object, properties) : 0;
}

int host_sdl_stop_text_input(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_StopTextInput(object) : 0;
}

int host_sdl_text_input_active(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_TextInputActive(object) : 0;
}

int host_sdl_has_screen_keyboard(void)
{
	return SDL_HasScreenKeyboardSupport() ? 1 : 0;
}

/* ---------- message boxes */

#define MESSAGE_BOX_BUTTONS 8

int host_sdl_show_message_box(uint32_t flags, const char *title, const char *message, int button_count,
	const int *button_flags, const int *button_ids, const char *button_texts, int *pushed)
{
	SDL_MessageBoxButtonData buttons[MESSAGE_BOX_BUTTONS];
	SDL_MessageBoxData data;
	const char *text = button_texts;
	int index;

	if (button_count > MESSAGE_BOX_BUTTONS)
		button_count = MESSAGE_BOX_BUTTONS;
	for (index = 0; index < button_count; index++)
	{
		buttons[index].flags = (SDL_MessageBoxButtonFlags)button_flags[index];
		buttons[index].buttonID = button_ids[index];
		buttons[index].text = text;
		text += strlen(text) + 1;
	}
	memset(&data, 0, sizeof(data));
	data.flags = (SDL_MessageBoxFlags)flags;
	data.title = title;
	data.message = message;
	data.numbuttons = button_count > 0 ? button_count : 0;
	data.buttons = buttons;
	return SDL_ShowMessageBox(&data, pushed) ? 1 : 0;
}

/* ---------- file dialogs

SDL answers on a thread of its own (or at once); the answer is kept here
until the guest asks for it (guest_sdl_desktop.c polls as it pumps the
events). */

#define DIALOG_REQUESTS 4
#define DIALOG_FILTERS 8

struct dialog_request
{
	int in_use;
	uint32_t number;
	/* 0 waiting, 1 chosen, 2 nothing chosen, 3 failed */
	int state;
	int filter;
	char *paths;
	size_t paths_size;
	SDL_DialogFileFilter filters[DIALOG_FILTERS];
	char names[512];
	char patterns[512];
	char location[1024];
};

static struct dialog_request dialog_requests[DIALOG_REQUESTS];
static pthread_mutex_t dialog_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t dialog_counter;

static void SDLCALL dialog_answered(void *userdata, const char *const *files, int filter)
{
	struct dialog_request *request = userdata;

	pthread_mutex_lock(&dialog_lock);
	request->filter = filter;
	if (!files)
	{
		request->state = 3;
	}
	else if (!files[0])
	{
		request->state = 2;
	}
	else
	{
		size_t size = 1;
		int index;

		for (index = 0; files[index]; index++)
			size += strlen(files[index]) + 1;
		request->paths = SDL_malloc(size);
		if (request->paths)
		{
			char *out = request->paths;

			for (index = 0; files[index]; index++)
			{
				strcpy(out, files[index]);
				out += strlen(files[index]) + 1;
			}
			*out = 0;
			request->paths_size = size;
			request->state = 1;
		}
		else
		{
			request->state = 3;
		}
	}
	pthread_mutex_unlock(&dialog_lock);
}

/* copies count strings (each ended by a NUL) into buffer; their starts into
starts; 0 if they do not fit */
static int strings_copy(const char *strings, int count, char *buffer, size_t size, const char **starts)
{
	size_t used = 0;
	int index;

	for (index = 0; index < count; index++)
	{
		size_t length = strlen(strings) + 1;

		if (used + length > size)
			return 0;
		memcpy(buffer + used, strings, length);
		starts[index] = buffer + used;
		used += length;
		strings += length;
	}
	return 1;
}

uint32_t host_sdl_show_open_file_dialog(uint32_t window, const char *filter_names, const char *filter_patterns,
	int filter_count, const char *default_location, int allow_many)
{
	struct dialog_request *request = NULL;
	const char *names[DIALOG_FILTERS], *patterns[DIALOG_FILTERS];
	int index;

	if (filter_count < 0 || filter_count > DIALOG_FILTERS)
		return 0;
	pthread_mutex_lock(&dialog_lock);
	for (index = 0; index < DIALOG_REQUESTS && !request; index++)
	{
		if (!dialog_requests[index].in_use)
			request = &dialog_requests[index];
	}
	if (!request || !strings_copy(filter_names, filter_count, request->names, sizeof(request->names), names) ||
		!strings_copy(filter_patterns, filter_count, request->patterns, sizeof(request->patterns), patterns))
	{
		pthread_mutex_unlock(&dialog_lock);
		return 0;
	}
	for (index = 0; index < filter_count; index++)
	{
		request->filters[index].name = names[index];
		request->filters[index].pattern = patterns[index];
	}
	SDL_strlcpy(request->location, default_location ? default_location : "", sizeof(request->location));
	request->in_use = 1;
	request->state = 0;
	request->filter = -1;
	request->paths = NULL;
	request->number = ++dialog_counter;
	pthread_mutex_unlock(&dialog_lock);
	SDL_ShowOpenFileDialog(dialog_answered, request, handle_get(window, _handle_window),
		filter_count ? request->filters : NULL, filter_count, request->location[0] ? request->location : NULL,
		allow_many != 0);
	return request->number;
}

int host_sdl_dialog_result(uint32_t number, char *buffer, uint32_t size, int *filter)
{
	int index, state = 3;

	pthread_mutex_lock(&dialog_lock);
	for (index = 0; index < DIALOG_REQUESTS; index++)
	{
		struct dialog_request *request = &dialog_requests[index];

		if (!request->in_use || request->number != number)
			continue;
		state = request->state;
		if (!state)
			break;
		*filter = request->filter;
		if (size >= 2)
		{
			buffer[0] = buffer[1] = 0;
			/* (the list cut where it does not fit: whole paths only) */
			if (state == 1)
			{
				const char *path = request->paths;
				size_t used = 0;

				while (*path && used + strlen(path) + 2 <= size)
				{
					strcpy(buffer + used, path);
					used += strlen(path) + 1;
					path += strlen(path) + 1;
				}
				buffer[used] = 0;
			}
		}
		SDL_free(request->paths);
		request->paths = NULL;
		request->in_use = 0;
		break;
	}
	pthread_mutex_unlock(&dialog_lock);
	return state;
}

/* ---------- renderers (the maps' extraction window) */

uint32_t host_sdl_create_renderer(uint32_t window, const char *name)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? handle_new(_handle_renderer, SDL_CreateRenderer(object, name)) : 0;
}

void host_sdl_destroy_renderer(uint32_t renderer)
{
	SDL_Renderer *object = handle_release(renderer, _handle_renderer, NULL);

	if (object)
		SDL_DestroyRenderer(object);
}

int host_sdl_set_render_vsync(uint32_t renderer, int vsync)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_SetRenderVSync(object, vsync) : 0;
}

int host_sdl_set_render_draw_color(uint32_t renderer, int red, int green, int blue, int alpha)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_SetRenderDrawColor(object, (Uint8)red, (Uint8)green, (Uint8)blue, (Uint8)alpha) : 0;
}

int host_sdl_render_clear(uint32_t renderer)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_RenderClear(object) : 0;
}

int host_sdl_set_render_scale(uint32_t renderer, float x, float y)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_SetRenderScale(object, x, y) : 0;
}

int host_sdl_render_debug_text(uint32_t renderer, float x, float y, const char *text)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_RenderDebugText(object, x, y, text) : 0;
}

int host_sdl_render_fill_rect(uint32_t renderer, const float *rectangle)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);
	SDL_FRect area;

	if (!object)
		return 0;
	area.x = rectangle[0];
	area.y = rectangle[1];
	area.w = rectangle[2];
	area.h = rectangle[3];
	return SDL_RenderFillRect(object, &area);
}

int host_sdl_render_present(uint32_t renderer)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_RenderPresent(object) : 0;
}

/* ---------- files */

void host_sdl_base_path(char *buffer, uint32_t size)
{
	snprintf(buffer, size, "%s/", host_data_root());
}

int host_sdl_glob_directory(const char *path, const char *pattern, uint32_t flags, char *buffer, uint32_t size)
{
	int count = 0, index;
	size_t used = 0;
	char **names = SDL_GlobDirectory(path, pattern, (SDL_GlobFlags)flags, &count);

	if (!names)
		return 0;
	for (index = 0; index < count; index++)
	{
		size_t length = strlen(names[index]) + 1;

		if (used + length > size)
		{
			SDL_free(names);
			return -1;
		}
		memcpy(buffer + used, names[index], length);
		used += length;
	}
	SDL_free(names);
	return count;
}
