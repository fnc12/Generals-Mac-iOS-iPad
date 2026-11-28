#pragma once

// A stand-in for the game app the Play button starts. It writes the arguments it got (one per line)
// to args.txt, CNC_GENERALS_ZH_PATH, CNC_GENERALS_PATH and GENERALSX_USER_DATA_DIR to env.txt and
// appends "started <pid>" to runs.txt, all next to the app; then it waits `seconds` in a child
// process (the way GeneralsX's run.sh runs the game as its child), writes the child's id to
// child.txt and exits with `code`. `ignoreStop`: it does not quit when asked (SIGTERM, on Windows
// WM_CLOSE). macOS and Linux: Fake.app/Contents/MacOS/run.sh; Windows: Fake.exe
// (tests/fake_game.cpp) with its fake.cfg.
#include "game_launch.h"

#include <filesystem>
#include <fstream>

#ifndef _WIN32
#include <csignal>
#endif

namespace end_editor_tests {

#ifdef _WIN32
    // What the game's exit code is when it quit because it was asked (its window closed).
    inline constexpr int stoppedExitCode = 3;
    inline constexpr int forcedExitCode = end_editor::killedExitCode;
#else
    inline constexpr int stoppedExitCode = 128 + SIGTERM;
    inline constexpr int forcedExitCode = 128 + SIGKILL;
#endif

    // The app to choose in the Play settings (play.app).
    inline std::filesystem::path
    makeFakeGame(const std::filesystem::path& folder, int seconds = 0, int code = 0, bool ignoreStop = false) {
#ifdef _WIN32
        const auto app = folder / "Fake.exe";
        std::filesystem::copy_file(END_EDITOR_FAKE_GAME, app, std::filesystem::copy_options::overwrite_existing);
        std::ofstream(folder / "fake.cfg") << seconds << ' ' << code << ' ' << (ignoreStop ? 1 : 0) << '\n';
        return app;
#else
        const auto app = folder / "Fake.app";
        std::filesystem::create_directories(app / "Contents" / "MacOS");
        const auto script = app / "Contents" / "MacOS" / "run.sh";
        std::ofstream(script) << "#!/bin/sh\n"
                              << (ignoreStop ? "trap '' TERM\n" : "")
                              << "for a in \"$@\"; do printf '%s\\n' \"$a\"; done > '" << (folder / "args.txt").string()
                              << "'\n"
                              << "printf '%s\\n%s\\n%s\\n' \"$CNC_GENERALS_ZH_PATH\" \"$CNC_GENERALS_PATH\" "
                                 "\"$GENERALSX_USER_DATA_DIR\" > '"
                              << (folder / "env.txt").string() << "'\n"
                              << "echo started $$ >> '" << (folder / "runs.txt").string() << "'\n"
                              << "sleep " << seconds << " &\necho $! > '" << (folder / "child.txt").string()
                              << "'\nwait $!\nexit " << code << "\n";
        std::filesystem::permissions(script, std::filesystem::perms::owner_all);
        return app;
#endif
    }

    // What the core starts for that app (programForApp).
    inline std::filesystem::path fakeGameProgram(const std::filesystem::path& app) {
#ifdef _WIN32
        return app;
#else
        return app / "Contents" / "MacOS" / "run.sh";
#endif
    }
}
