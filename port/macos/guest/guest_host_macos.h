/*
GUEST_HOST_MACOS.H

The host services the macOS guest imports besides the Android guest's
(port/android/guest/runtime/guest_host.h): the desktop's SDL, which the
platform layer's desktop code calls (port/linux/src/sdl_platform.c), through
guest_sdl_desktop.c. Every name here must also be listed in
port/macos/host_imports.list.

The parameter types follow guest_host.h's rules. A pointer parameter is
widened to the host's address of the guest memory it points at by its
import stub (tools/macos_imports.py), so the host takes it as a pointer.
Windows, renderers and the rest are small integer handles, as in
guest_sdl.c. A display mode crosses as its first 32 bytes, which the two
ABIs lay out alike (its last member, a pointer, stays behind).
*/

#ifndef __GUEST_HOST_MACOS_H
#define __GUEST_HOST_MACOS_H

/* ---------- time and hints */

long long host_sdl_ticks_ns(void);
void host_sdl_delay_precise(long long nanoseconds);
int host_sdl_get_hint_boolean(const char *name, int default_value);

/* ---------- windows */

void host_sdl_destroy_window(unsigned int window);
long long host_sdl_window_flags(unsigned int window);
void host_sdl_window_size(unsigned int window, int *width, int *height);
int host_sdl_set_window_size(unsigned int window, int width, int height);
int host_sdl_set_window_fullscreen(unsigned int window, int fullscreen);
/* mode: a display mode's first 32 bytes, or NULL for the desktop's */
int host_sdl_set_window_fullscreen_mode(unsigned int window, const void *mode);
void host_sdl_warp_mouse(unsigned int window, float x, float y);

/* ---------- displays (a mode: its first 32 bytes, into mode) */

unsigned int host_sdl_primary_display(void);
unsigned int host_sdl_display_for_window(unsigned int window);
int host_sdl_display_usable_bounds(unsigned int display, int *rectangle);
int host_sdl_desktop_display_mode(unsigned int display, void *mode);
int host_sdl_current_display_mode(unsigned int display, void *mode);
int host_sdl_closest_display_mode(unsigned int display, int width, int height, float refresh_rate,
	int include_high_density, void *mode);
/* up to capacity modes, 32 bytes each, into modes; their count */
int host_sdl_fullscreen_display_modes(unsigned int display, void *modes, int capacity);

/* ---------- audio devices */

/* up to capacity device numbers into ids; their count */
int host_sdl_audio_devices(int recording, unsigned int *ids, int capacity);
/* the device's name copied into buffer; 0 if it has none */
int host_sdl_audio_device_name(unsigned int device, char *buffer, unsigned int size);

/* ---------- events and text input */

void host_sdl_pump_events(void);
/* event: an SDL_Event without pointers in it */
int host_sdl_push_event(const void *event);
unsigned int host_sdl_create_properties(void);
void host_sdl_destroy_properties(unsigned int properties);
int host_sdl_set_boolean_property(unsigned int properties, const char *name, int value);
int host_sdl_set_number_property(unsigned int properties, const char *name, long long value);
int host_sdl_start_text_input(unsigned int window, unsigned int properties);
int host_sdl_stop_text_input(unsigned int window);
int host_sdl_text_input_active(unsigned int window);
int host_sdl_has_screen_keyboard(void);

/* ---------- message boxes and dialogs */

/* buttons: their flags, numbers and texts (one after another, each ended by
a NUL); the button pushed into pushed */
int host_sdl_show_message_box(unsigned int flags, const char *title, const char *message, int button_count,
	const int *button_flags, const int *button_ids, const char *button_texts, int *pushed);
/* opens the system's file picker; a request number to poll
(host_sdl_dialog_result), 0 if it cannot open. Filters: their names and
patterns, each ended by a NUL */
unsigned int host_sdl_show_open_file_dialog(unsigned int window, const char *filter_names,
	const char *filter_patterns, int filter_count, const char *default_location, int allow_many);
/* 0 while the request waits; 1 once chosen, with the paths in buffer (each
ended by a NUL, then an empty one) and the filter in *filter; 2 if nothing
was chosen; 3 if the picker failed. The request ends with an answer other
than 0. */
int host_sdl_dialog_result(unsigned int request, char *buffer, unsigned int size, int *filter);

/* ---------- renderers (the maps' extraction window) */

unsigned int host_sdl_create_renderer(unsigned int window, const char *name);
void host_sdl_destroy_renderer(unsigned int renderer);
int host_sdl_set_render_vsync(unsigned int renderer, int vsync);
int host_sdl_set_render_draw_color(unsigned int renderer, int red, int green, int blue, int alpha);
int host_sdl_render_clear(unsigned int renderer);
int host_sdl_set_render_scale(unsigned int renderer, float x, float y);
int host_sdl_render_debug_text(unsigned int renderer, float x, float y, const char *text);
/* rectangle: x, y, width, height */
int host_sdl_render_fill_rect(unsigned int renderer, const float *rectangle);
int host_sdl_render_present(unsigned int renderer);

/* ---------- files */

/* the data folder, ending in its separator (SDL_GetBasePath for config.toml
and menus/: port/macos/host/host_main.c) */
void host_sdl_base_path(char *buffer, unsigned int size);
/* the names in path matching pattern, each ended by a NUL, into buffer;
their count, or -1 if they do not fit */
int host_sdl_glob_directory(const char *path, const char *pattern, unsigned int flags, char *buffer,
	unsigned int size);

#endif
