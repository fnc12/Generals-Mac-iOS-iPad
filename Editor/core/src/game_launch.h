#pragma once

#include <cstdint>
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
    // (an executable or script, e.g. on Linux). "" when there is nothing to start.
    std::string programForApp(const std::string& app);

    Launched launchProcess(const LaunchRequest& request);

    // nullopt while it runs; its exit code once it ended (128 + signal when killed by one).
    std::optional<int> pollProcess(std::int64_t pid);

    // Asks it and the processes it started to quit (SIGTERM), or ends them (SIGKILL) when `force`.
    void stopProcess(std::int64_t pid, bool force = false);
}
