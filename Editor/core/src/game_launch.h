#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace end_editor {

    // Starting the game for the Play button (map.play), on its own: it keeps running when the
    // editor quits, its output goes to a log file.
    struct LaunchRequest {
        std::string program;
        std::vector<std::string> arguments;
        std::vector<std::pair<std::string, std::string>> environment;  // set on top of the editor's
        std::string logPath;
    };

    struct Launched {
        std::int64_t pid = 0;
        std::string error;  // "" when started
    };

    // What starts a game app: <app>/Contents/MacOS/run.sh (GeneralsX's launcher, which sets the
    // runtime's library and Vulkan paths), else the bundle's CFBundleExecutable, else `app` itself
    // (an executable or script, e.g. on Linux). On Windows `app` is the game's .exe (or .com), or
    // a folder with <folder name>.exe, GeneralsXZH.exe or GeneralsX.exe in it. "" when there is
    // nothing to start.
    std::string programForApp(const std::string& app);

    // The user's home folder: HOME, on Windows USERPROFILE; "" when not set.
    std::string homeFolder();

    // Whether a user Maps folder is the base game's: its user data folder is GeneralsX's "Generals"
    // (macOS, Linux) or the original game's "Command and Conquer Generals Data" (Windows).
    bool isBaseGameUserMaps(const std::filesystem::path& userMaps);

    // The game app Play starts when none is chosen, in the user's GeneralsX folder:
    // GeneralsX/Apps/GeneralsXZH.app (GeneralsX.app for the base game), on Windows
    // GeneralsX\Apps\GeneralsXZH\GeneralsXZH.exe (GeneralsX\GeneralsX.exe), the layout of
    // scripts/mods/install-windows-launchers.ps1. "" without a home folder.
    std::string defaultGameApp(const std::string& home, bool baseGame);

    Launched launchProcess(const LaunchRequest& request);

    // nullopt while it runs; its exit code once it ended (128 + signal when killed by one; on
    // Windows `killedExitCode` when ended by force).
    std::optional<int> pollProcess(std::int64_t pid);

    // Asks it and the processes it started to quit (SIGTERM; on Windows WM_CLOSE to their windows),
    // or ends them (SIGKILL; on Windows the whole Job object) when `force`.
    void stopProcess(std::int64_t pid, bool force = false);

    // Whether a process with this id runs (tests and checks).
    bool processRuns(std::int64_t pid);

    // The exit code of a game ended by force on Windows (TerminateJobObject).
    inline constexpr int killedExitCode = 1;
}
