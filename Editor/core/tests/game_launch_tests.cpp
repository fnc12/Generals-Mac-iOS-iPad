// Starting, watching and stopping the game for the Play button (game_launch.h), on every platform:
// posix_spawn and process groups on macOS and Linux, CreateProcessW and a Job object on Windows.
#include "game_launch.h"
#include "test_big_writer.h"
#include "test_fake_game.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace end_editor;
using namespace end_editor_tests;

namespace {
    std::string readFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    std::vector<std::string> lines(const std::string& text) {
        std::vector<std::string> result;
        std::size_t start = 0;
        while (start < text.size()) {
            const auto end = text.find('\n', start);
            result.push_back(text.substr(start, end - start));
            start = end == std::string::npos ? text.size() : end + 1;
        }
        return result;
    }

    // Polls until the process ended; nullopt when it still runs after `seconds`.
    std::optional<int> waitFor(std::int64_t pid, double seconds = 10) {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::duration<double>(seconds)) {
            if (const auto code = pollProcess(pid)) {
                return code;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return std::nullopt;
    }
}

TEST_CASE("Launch: the game gets exactly its arguments and environment, its output goes to the log") {
    // A folder outside the system's code page: the paths cross the C ABI as UTF-8.
    const auto folder = makeTempFolder("launch-документы");
    const auto app = makeFakeGame(folder, 0, 7);
    const auto program = programForApp(app.string());
    REQUIRE(program == fakeGameProgram(app).string());

    LaunchRequest request;
    request.program = program;
    // Spaces, quotes, backslashes before a quote and at the end, an empty one, non-ASCII text.
    request.arguments = {"-testMap",
                         (folder / "My Map" / "My Map.map").string(),
                         "say \"hi\"",
                         "back\\\\\"slash",
                         "ends with\\",
                         "",
                         "Старт 1"};
    request.environment = {{"CNC_GENERALS_ZH_PATH", (folder / "Zero Hour").string()},
                           {"CNC_GENERALS_PATH", "base"},
                           {"GENERALSX_USER_DATA_DIR", folder.string()}};
    request.logPath = (folder / "play.log").string();
    const auto launched = launchProcess(request);
    REQUIRE(launched.error.empty());
    REQUIRE(launched.pid > 0);
    CHECK(waitFor(launched.pid) == 7);
    CHECK(lines(readFile(folder / "args.txt")) == request.arguments);
    CHECK(lines(readFile(folder / "env.txt")) ==
          std::vector<std::string>{(folder / "Zero Hour").string(), "base", folder.string()});
    CHECK(std::filesystem::exists(folder / "play.log"));
    // Reported once; a process the editor does not know (any more) reads as ended.
    CHECK(pollProcess(launched.pid) == 0);
    CHECK_FALSE(processRuns(launched.pid));
}

TEST_CASE("Launch: a game that is not there is an error, not a process") {
    const auto folder = makeTempFolder("launch-missing");
    LaunchRequest request;
    request.program = (folder / "nothing" / "Game.exe").string();
    const auto launched = launchProcess(request);
    CHECK(launched.pid == 0);
    CHECK(launched.error.rfind("cannot start " + request.program, 0) == 0);
}

TEST_CASE("Launch: stopping asks the game to quit; forced, its whole process tree ends") {
    const auto folder = makeTempFolder("launch-stop");
    LaunchRequest request;
    request.program = programForApp(makeFakeGame(folder, 30).string());
    request.logPath = (folder / "play.log").string();
    auto launched = launchProcess(request);
    REQUIRE(launched.error.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // its child and window are up
    CHECK(processRuns(launched.pid));
    CHECK_FALSE(pollProcess(launched.pid).has_value());
    const auto child = std::stoll(readFile(folder / "child.txt"));
    CHECK(processRuns(child));
    stopProcess(launched.pid);
    CHECK(waitFor(launched.pid) == stoppedExitCode);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_FALSE(processRuns(child));  // the game it ran went with it

    // One that ignores the request is ended by force, with what it started.
    std::filesystem::remove(folder / "child.txt");
    request.program = programForApp(makeFakeGame(folder, 30, 0, true).string());
    launched = launchProcess(request);
    REQUIRE(launched.error.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto stubbornChild = std::stoll(readFile(folder / "child.txt"));
    stopProcess(launched.pid);
    CHECK_FALSE(waitFor(launched.pid, 1).has_value());
    stopProcess(launched.pid, true);
    CHECK(waitFor(launched.pid) == forcedExitCode);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_FALSE(processRuns(stubbornChild));
}

TEST_CASE("Launch: what starts a game app, the default app and the base game's user Maps") {
    const auto folder = makeTempFolder("launch-apps");
    CHECK(programForApp("").empty());
    CHECK(programForApp((folder / "missing").string()).empty());
#ifdef _WIN32
    // An .exe, or a folder with <folder>.exe, GeneralsXZH.exe or GeneralsX.exe; nothing else runs.
    std::filesystem::create_directories(folder / "GeneralsXZH");
    std::ofstream(folder / "GeneralsXZH" / "GeneralsXZH.exe") << "MZ";
    CHECK(programForApp((folder / "GeneralsXZH").string()) == (folder / "GeneralsXZH" / "GeneralsXZH.exe").string());
    std::filesystem::create_directories(folder / "Build");
    std::ofstream(folder / "Build" / "GeneralsX.exe") << "MZ";
    CHECK(programForApp((folder / "Build").string()) == (folder / "Build" / "GeneralsX.exe").string());
    std::ofstream(folder / "run.bat") << "@echo off";
    CHECK(programForApp((folder / "run.bat").string()).empty());
    CHECK(programForApp((folder / "Build" / "GeneralsX.exe").string()) ==
          (folder / "Build" / "GeneralsX.exe").string());
    CHECK(defaultGameApp("C:\\Users\\me", false) == "C:\\Users\\me\\GeneralsX\\Apps\\GeneralsXZH\\GeneralsXZH.exe");
    CHECK(defaultGameApp("C:\\Users\\me", true) == "C:\\Users\\me\\GeneralsX\\Apps\\GeneralsX\\GeneralsX.exe");
#else
    // A bundle's run.sh, else its CFBundleExecutable; or an executable itself.
    const auto app = makeFakeGame(folder);
    CHECK(programForApp(app.string()) == (app / "Contents" / "MacOS" / "run.sh").string());
    std::filesystem::create_directories(folder / "Plain.app" / "Contents" / "MacOS");
    std::ofstream(folder / "Plain.app" / "Contents" / "Info.plist")
        << "<plist><dict><key>CFBundleExecutable</key><string>Game</string></dict></plist>";
    const auto executable = folder / "Plain.app" / "Contents" / "MacOS" / "Game";
    std::ofstream(executable) << "#!/bin/sh\n";
    std::filesystem::permissions(executable, std::filesystem::perms::owner_all);
    CHECK(programForApp((folder / "Plain.app").string()) == executable.string());
    CHECK(programForApp(executable.string()) == executable.string());
    CHECK(defaultGameApp("/Users/me", false) == "/Users/me/GeneralsX/Apps/GeneralsXZH.app");
    CHECK(defaultGameApp("/Users/me", true) == "/Users/me/GeneralsX/Apps/GeneralsX.app");
#endif
    CHECK(defaultGameApp("", false).empty());
    CHECK(isBaseGameUserMaps(folder / "GeneralsX" / "Generals" / "Maps"));
    CHECK(isBaseGameUserMaps(folder / "Command and Conquer Generals Data" / "Maps"));
    CHECK_FALSE(isBaseGameUserMaps(folder / "GeneralsX" / "GeneralsZH" / "Maps"));
    CHECK_FALSE(isBaseGameUserMaps(folder / "Command and Conquer Generals Zero Hour Data" / "Maps"));
}
