#include "game_launch.h"

#include <filesystem>
#include <fstream>
#include <iterator>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

namespace end_editor {

    namespace {
        bool isExecutable(const std::filesystem::path &path) {
            std::error_code error;
            if(!std::filesystem::is_regular_file(path, error)) {
                return false;
            }
#ifdef _WIN32
            return true;
#else
            return ::access(path.c_str(), X_OK) == 0;
#endif
        }

        // <key>CFBundleExecutable</key> <string>name</string> of a bundle's Info.plist (XML).
        std::string bundleExecutable(const std::filesystem::path &plist) {
            std::ifstream file(plist, std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            const auto key = text.find("<key>CFBundleExecutable</key>");
            if(key == std::string::npos) {
                return "";
            }
            const auto open = text.find("<string>", key);
            const auto close = open == std::string::npos ? std::string::npos : text.find("</string>", open);
            return close == std::string::npos ? "" : text.substr(open + 8, close - open - 8);
        }
    }

    std::string programForApp(const std::string &app) {
        if(app.empty()) {
            return "";
        }
        const std::filesystem::path path(app);
        std::error_code error;
        if(std::filesystem::is_directory(path, error)) {
            const auto macOS = path / "Contents" / "MacOS";
            if(isExecutable(macOS / "run.sh")) {
                return (macOS / "run.sh").string();
            }
            const auto name = bundleExecutable(path / "Contents" / "Info.plist");
            if(!name.empty() && isExecutable(macOS / name)) {
                return (macOS / name).string();
            }
            return "";
        }
        return isExecutable(path) ? path.string() : "";
    }

#ifdef _WIN32
    Launched launchProcess(const LaunchRequest &) {
        return Launched{0, "starting the game is not supported on Windows yet"};
    }
    std::optional<int> pollProcess(std::int64_t) {
        return 0;
    }
    void stopProcess(std::int64_t) {}
#else
    Launched launchProcess(const LaunchRequest &request) {
        std::vector<std::string> environment;
        for(char **entry = environ; entry && *entry; ++entry) {
            const std::string text(*entry);
            const auto name = text.substr(0, text.find('='));
            bool replaced = false;
            for(const auto &[key, value]: request.environment) {
                replaced = replaced || key == name;
            }
            if(!replaced) {
                environment.push_back(text);
            }
        }
        for(const auto &[key, value]: request.environment) {
            environment.push_back(key + "=" + value);
        }
        std::vector<char *> envp;
        for(auto &entry: environment) {
            envp.push_back(entry.data());
        }
        envp.push_back(nullptr);

        std::vector<std::string> arguments{request.program};
        arguments.insert(arguments.end(), request.arguments.begin(), request.arguments.end());
        std::vector<char *> argv;
        for(auto &argument: arguments) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        const std::string log = request.logPath.empty() ? std::string("/dev/null") : request.logPath;
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        // Its own session: no signal of the editor's terminal or process group reaches it.
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
        pid_t pid = 0;
        const int result = posix_spawn(&pid, request.program.c_str(), &actions, &attributes, argv.data(), envp.data());
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        if(result != 0) {
            return Launched{0, "cannot start " + request.program + ": " + std::strerror(result)};
        }
        return Launched{pid, ""};
    }

    std::optional<int> pollProcess(std::int64_t pid) {
        if(pid <= 0) {
            return 0;
        }
        int status = 0;
        const auto done = ::waitpid(pid_t(pid), &status, WNOHANG);
        if(done == 0) {
            return std::nullopt;
        }
        if(done < 0) {
            return errno == ECHILD ? 0 : -1;
        }
        if(WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    }

    void stopProcess(std::int64_t pid) {
        if(pid > 0) {
            ::kill(pid_t(pid), SIGTERM);
        }
    }
#endif
}
