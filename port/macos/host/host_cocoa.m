/*
HOST_COCOA.M

The window's layers (host_sdl.c calls it once the game's OpenGL ES context
is made), the windows' restoration, off (host_main.c, before SDL starts
AppKit), and the answers to Accessibility's questions (below).

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
#import <objc/runtime.h>

#include <SDL3/SDL.h>
#include <pthread.h>
#include <time.h>

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

/* AppKit's restoration of the app's windows at its start, off: the game
makes its own window, and after a crash AppKit would first ask whether to
reopen the windows, in an alert that holds the start until it is answered
(and that a full screen game's window can hide) */
void host_cocoa_no_window_restoration(void)
{
	@autoreleasepool
	{
		[[NSUserDefaults standardUserDefaults] registerDefaults:@{
			@"ApplePersistenceIgnoreState": @YES,
			@"NSQuitAlwaysKeepsWindows": @NO,
		}];
	}
}

/* ---------- Accessibility's questions

Accessibility's clients ask the app and its windows questions (a window's
role, place, size), which AppKit answers on the main thread, in its run
loop: in the game, when it pumps the events, once a frame. A client that
asks from an event tap holds the event until it has its answers, and the
events behind it wait too. Magnet asks some twenty questions on each left
click (whether the click starts a window's drag): at 120 frames a second,
the click reached the game about 140 ms late, and the mouse's motion with
it (the aim stopped at the click, then caught up).

AppKit answers through the app's, windows' and views'
accessibilityAttributeValue: and the app's accessibilityHitTest:, which
count the questions here. After a pump that answered one, the main thread
goes on answering as they come, so that a run of questions costs about a
frame instead of a frame each. */

static unsigned long accessibility_questions;
static uint64_t accessibility_asked_ns;
static IMP application_value, window_value, view_value, application_hit_test;

static void accessibility_asked(void)
{
	accessibility_questions++;
	accessibility_asked_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static id application_value_counted(id self, SEL command, NSString *attribute)
{
	accessibility_asked();
	return ((id (*)(id, SEL, NSString *))application_value)(self, command, attribute);
}

static id window_value_counted(id self, SEL command, NSString *attribute)
{
	accessibility_asked();
	return ((id (*)(id, SEL, NSString *))window_value)(self, command, attribute);
}

static id view_value_counted(id self, SEL command, NSString *attribute)
{
	accessibility_asked();
	return ((id (*)(id, SEL, NSString *))view_value)(self, command, attribute);
}

static id application_hit_test_counted(id self, SEL command, NSPoint point)
{
	accessibility_asked();
	return ((id (*)(id, SEL, NSPoint))application_hit_test)(self, command, point);
}

/* counted replaces the class's own method, or is added to the class over
the one that it inherits; *original is the method that counted calls */
static void accessibility_count(Class class, SEL selector, IMP counted, IMP *original)
{
	Method method = class_getInstanceMethod(class, selector);

	if (!method)
	{
		host_logf(HOST_LOG_WARN, "accessibility: no -[%s %s]", class_getName(class), sel_getName(selector));
		return;
	}
	*original = method_getImplementation(method);
	if (!class_addMethod(class, selector, counted, method_getTypeEncoding(method)))
		method_setImplementation(method, counted);
}

void host_cocoa_accessibility_watch(void)
{
	accessibility_count([NSApplication class], @selector(accessibilityAttributeValue:),
		(IMP)application_value_counted, &application_value);
	accessibility_count([NSWindow class], @selector(accessibilityAttributeValue:),
		(IMP)window_value_counted, &window_value);
	accessibility_count([NSView class], @selector(accessibilityAttributeValue:),
		(IMP)view_value_counted, &view_value);
	accessibility_count([NSApplication class], @selector(accessibilityHitTest:),
		(IMP)application_hit_test_counted, &application_hit_test);
}

void host_cocoa_accessibility_answer(void)
{
	static unsigned long answered;
	uint64_t start, now;

	if (accessibility_questions == answered || !pthread_main_np())
		return;
	/* a client asks its next question when it has the answer to the last:
	answered while they come, until 1 ms passes without one, for 4 ms at
	most (the frame's time) */
	start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	for (now = start; now - start < 4000000 && now - accessibility_asked_ns < 1000000;
		now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW))
	{
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0005, true);
	}
	answered = accessibility_questions;
}
