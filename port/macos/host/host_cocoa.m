/*
HOST_COCOA.M

The window's layers (host_sdl.c calls it once the game's OpenGL ES context
is made).

ANGLE draws the game into a CAMetalLayer of its own, which it adds to the
layer of the window's view (SDL hands it that layer for the EGL surface).
The window server blends a layer by its alpha unless the layer is opaque,
and the game keeps scratch values in destination alpha (zero in most of the
picture): with a layer that is not opaque the window showed black, the
picture's alpha taking it away. The game asks for a surface without alpha
(sdl_platform.c), and every CAMetalLayer under the view is made opaque here
whatever ANGLE chose, as a display's ignores alpha elsewhere.
*/

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include <SDL3/SDL.h>

#include "host.h"

static int layers_make_opaque(CALayer *layer, int depth)
{
	int found = 0;

	if ([layer isKindOfClass:[CAMetalLayer class]])
	{
		CAMetalLayer *metal = (CAMetalLayer *)layer;

		host_logf(HOST_LOG_INFO, "window layer: Metal, %.0fx%.0f pixels at scale %.1f, pixel format %lu, %s",
			metal.drawableSize.width, metal.drawableSize.height, metal.contentsScale,
			(unsigned long)metal.pixelFormat, metal.opaque ? "opaque" : "not opaque: made opaque");
		metal.opaque = YES;
		found++;
	}
	if (depth < 4)
	{
		for (CALayer *sublayer in layer.sublayers)
			found += layers_make_opaque(sublayer, depth + 1);
	}
	return found;
}

void host_cocoa_window_opaque(SDL_Window *window)
{
	NSWindow *nswindow = (__bridge NSWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
		SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
	NSView *view = nswindow.contentView;

	if (!view.layer)
	{
		host_logf(HOST_LOG_WARN, "window layer: the window's view has none");
		return;
	}
	if (!layers_make_opaque(view.layer, 0))
		host_logf(HOST_LOG_WARN, "window layer: no Metal layer under the window's view");
	view.layer.opaque = YES;
}
