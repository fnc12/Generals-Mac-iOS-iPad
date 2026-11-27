// GeneralsX @feature fnc12 27/11/2026 Native macOS trackpad camera control, shared by Generals and
// Zero Hour.
//
// SDL3 turns every scroll into SDL_EVENT_MOUSE_WHEEL and has no "precise" flag, so a trackpad
// two-finger swipe and a mouse wheel notch look alike to the game (both zoomed). On macOS the real
// signal is NSEvent.hasPreciseScrollingDeltas. An NSEvent local monitor (MacTrackpad.mm) takes the
// precise scroll events of the game window before SDL sees them and posts them as an SDL event of
// our own type carrying NSEvent.scrollingDeltaX/Y (window points, already following the user's
// natural-scrolling setting: the content moves by +dx right, +dy down). Line-based mouse wheel
// events are left alone and keep zooming exactly as before.
//
// Pinch needs nothing here: SDL 3.4 sends SDL_EVENT_PINCH_* itself.
//
// Built for macOS only (Core/GameEngineDevice/CMakeLists.txt); elsewhere these are inline no-ops
// so callers compile unchanged and Windows/Linux/iOS behave as before.
#pragma once

#include <SDL3/SDL.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_OSX
#define GENERALSX_MAC_TRACKPAD 1
#else
#define GENERALSX_MAC_TRACKPAD 0
#endif

namespace MacTrackpad
{

#if GENERALSX_MAC_TRACKPAD

/// Installs the scroll monitor once (call after SDL video init; repeated calls are cheap).
void install();

/// The SDL event type of a trackpad pan, or 0 before install().
Uint32 panEventType();

/// Reads a pan event: dx/dy in window points the content should move. False for other events.
bool decodePan(const SDL_Event &event, float &dx, float &dy);

#else

inline void install() {}
inline Uint32 panEventType() { return 0; }
inline bool decodePan(const SDL_Event &, float &, float &) { return false; }

#endif

} // namespace MacTrackpad
