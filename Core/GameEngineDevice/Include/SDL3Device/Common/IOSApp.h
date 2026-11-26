// GeneralsX @feature fnc12 26/11/2026 iOS app support shared by Generals and Zero Hour.
// Both games run the same iOS shell: the process setup in main(), the app lifecycle and
// the touch -> mouse translation all live here once; each game's SDL3Main.cpp and
// SDL3GameEngine.cpp only call in. Built for iOS only (Core/GameEngineDevice/CMakeLists.txt).
#pragma once

#include <SDL3/SDL.h>

namespace IOSApp
{

/// What differs between the two games.
struct Game
{
	const char *userDataName;            ///< "Generals" or "GeneralsZH" (Library/Application Support/GeneralsX/<name>)
	const char *executableName;          ///< "GeneralsX" or "GeneralsXZH" (DXVK cache and log file names)
	const char *const *extraShippedAssets; ///< nullptr-terminated top-level names only this game ships, or nullptr
};

/// Before anything else in main(): the stderr log in Documents, the working directory
/// (<bundle>/GameData, or Documents for asset-less dev builds), the DXVK shader cache,
/// default Options.ini on first run and the one-time tidy-up of old asset copies.
void prepareProcess(int argc, char *argv[], const Game &game);

/// SDL hints to set before SDL_InitSubSystem(SDL_INIT_VIDEO).
void setSDLHints();

/// After the window exists: append -xres/-yres for the screen's pixel size (aspect capped
/// at 16:9) unless the user passed them. Rewrites argc/argv.
void appendScreenResolution(SDL_Window *window, int &argc, char **&argv);

/// In the engine's init(): start tracking background/inactive state.
void installLifecycleWatcher();

/// True while iOS owns the window (backgrounded or inactive): do not render then.
bool shouldPauseRendering();

/// Where synthesized mouse events go (the game's SDL3Mouse::addSDLEvent).
typedef void (*MouseSink)(SDL_Event *event, void *context);

/// Feed every SDL finger event here; it becomes mouse input for the sink.
void handleTouchEvent(SDL_Window *window, const SDL_Event &event, MouseSink sink, void *context);

/// Call once per frame: a still finger sends no events, so the long press is timed here.
void updateTouch(SDL_Window *window, MouseSink sink, void *context);

} // namespace IOSApp
