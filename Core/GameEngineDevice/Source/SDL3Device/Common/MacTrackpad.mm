// GeneralsX @feature fnc12 27/11/2026 Native macOS trackpad camera control. See MacTrackpad.h.
#import <AppKit/AppKit.h>

#include "SDL3Device/Common/MacTrackpad.h"
#include "GameClient/TrackpadGesture.h"

#include <cstdint>
#include <cstring>

namespace
{
Uint32 s_panEventType = 0;
id s_monitor = nil;

/// The SDL window the scroll event belongs to, or nullptr for windows that are not the game's.
SDL_Window *gameWindowFor(NSEvent *event)
{
	NSWindow *eventWindow = [event window];
	if (eventWindow == nil)
		return nullptr;

	int count = 0;
	SDL_Window **windows = SDL_GetWindows(&count);
	SDL_Window *found = nullptr;
	for (int i = 0; windows != nullptr && i < count && found == nullptr; ++i)
	{
		void *cocoa = SDL_GetPointerProperty(SDL_GetWindowProperties(windows[i]), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
		if (cocoa == (__bridge void *)eventWindow)
			found = windows[i];
	}
	SDL_free(windows);
	return found;
}

Sint32 floatBits(float value)
{
	Sint32 bits;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

float bitsFloat(Sint32 bits)
{
	float value;
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}
} // namespace

namespace MacTrackpad
{

void install()
{
	if (s_monitor != nil || NSApp == nil)
		return;

	if (s_panEventType == 0)
	{
		s_panEventType = SDL_RegisterEvents(1);
		if (s_panEventType == 0)
			return;
	}

	s_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel handler:^NSEvent *(NSEvent *event) {
		// A line-based mouse wheel: SDL keeps it, the game zooms as always.
		if (!TrackpadGesture::isPanScroll([event hasPreciseScrollingDeltas]))
			return event;

		SDL_Window *window = gameWindowFor(event);
		if (window == nullptr)
			return event;

		// Trackpad (also its momentum after the fingers lift): pan. SDL must not see it, or it
		// would also arrive as a wheel event and zoom.
		const float dx = (float)[event scrollingDeltaX];
		const float dy = (float)[event scrollingDeltaY];
		if (TrackpadGesture::hasPanMovement(dx, dy))
		{
			SDL_Event pan;
			SDL_zero(pan);
			pan.type = s_panEventType;
			pan.user.windowID = SDL_GetWindowID(window);
			pan.user.code = floatBits(dx);
			pan.user.data1 = (void *)(uintptr_t)(uint32_t)floatBits(dy);
			SDL_PushEvent(&pan);
		}
		return nil;
	}];
#if !__has_feature(objc_arc)
	[s_monitor retain];
#endif
}

Uint32 panEventType()
{
	return s_panEventType;
}

bool decodePan(const SDL_Event &event, float &dx, float &dy)
{
	if (s_panEventType == 0 || event.type != s_panEventType)
		return false;
	dx = bitsFloat(event.user.code);
	dy = bitsFloat((Sint32)(uint32_t)(uintptr_t)event.user.data1);
	return true;
}

} // namespace MacTrackpad
