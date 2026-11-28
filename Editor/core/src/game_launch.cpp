#include "game_launch.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <map>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace end_editor {

    namespace {
        bool isExecutable(const std::filesystem::path& path) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error)) {
                return false;
            }
#ifdef _WIN32
            // What CreateProcess starts by itself (a script would need cmd.exe and its quoting).
            auto extension = path.extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) {
                return wchar_t(std::towlower(c));
            });
            return extension == L".exe" || extension == L".com";
#else
            return ::access(path.c_str(), X_OK) == 0;
#endif
        }

#ifndef _WIN32
        // <key>CFBundleExecutable</key> <string>name</string> of a bundle's Info.plist (XML).
        std::string bundleExecutable(const std::filesystem::path& plist) {
            std::ifstream file(plist, std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            const auto key = text.find("<key>CFBundleExecutable</key>");
            if (key == std::string::npos) {
                return "";
            }
            const auto open = text.find("<string>", key);
            const auto close = open == std::string::npos ? std::string::npos : text.find("</string>", open);
            return close == std::string::npos ? "" : text.substr(open + 8, close - open - 8);
        }
#endif
    }

    std::string programForApp(const std::string& app) {
        if (app.empty()) {
            return "";
        }
        const std::filesystem::path path(app);
        std::error_code error;
        if (std::filesystem::is_directory(path, error)) {
#ifdef _WIN32
            for (const auto& name:
                 {path.filename().string() + ".exe", std::string("GeneralsXZH.exe"), std::string("GeneralsX.exe")}) {
                if (isExecutable(path / name)) {
                    return (path / name).string();
                }
            }
            return "";
#else
            const auto macOS = path / "Contents" / "MacOS";
            if (isExecutable(macOS / "run.sh")) {
                return (macOS / "run.sh").string();
            }
            const auto name = bundleExecutable(path / "Contents" / "Info.plist");
            if (!name.empty() && isExecutable(macOS / name)) {
                return (macOS / name).string();
            }
            return "";
#endif
        }
        return isExecutable(path) ? path.string() : "";
    }

#ifndef _WIN32
    std::string homeFolder() {
        const char* home = std::getenv("HOME");
        return home ? home : "";
    }
#endif

    bool isBaseGameUserMaps(const std::filesystem::path& userMaps) {
        const auto data = userMaps.parent_path().filename();
        return data == "Generals" || data == "Command and Conquer Generals Data";
    }

    std::string defaultGameApp(const std::string& home, bool baseGame) {
        if (home.empty()) {
            return "";
        }
        const auto apps = std::filesystem::path(home) / "GeneralsX" / "Apps";
#ifdef _WIN32
        const char* name = baseGame ? "GeneralsX" : "GeneralsXZH";
        return (apps / name / (std::string(name) + ".exe")).string();
#else
        return (apps / (baseGame ? "GeneralsX.app" : "GeneralsXZH.app")).string();
#endif
    }

#ifdef _WIN32
    namespace {
        std::wstring widen(const std::string& text) {
            if (text.empty()) {
                return {};
            }
            const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
            std::wstring result(std::size_t(size), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), result.data(), size);
            return result;
        }

        std::string narrow(const wchar_t* text, int size) {
            const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text, size, nullptr, 0, nullptr, nullptr);
            std::string result(std::size_t(bytes), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, text, size, result.data(), bytes, nullptr, nullptr);
            return result;
        }

        std::string lastError() {
            const DWORD code = ::GetLastError();
            wchar_t* text = nullptr;
            ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr,
                             code,
                             MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                             reinterpret_cast<wchar_t*>(&text),
                             0,
                             nullptr);
            std::string result = "error " + std::to_string(code);
            if (text) {
                auto message = narrow(text, int(std::wcslen(text)));
                ::LocalFree(text);
                while (!message.empty() &&
                       (message.back() == '\n' || message.back() == '\r' || message.back() == '.')) {
                    message.pop_back();
                }
                result = message + " (" + result + ")";
            }
            return result;
        }

        // One argument as CommandLineToArgvW and the C runtime read it back.
        void appendArgument(std::wstring& line, const std::wstring& argument) {
            if (!line.empty()) {
                line.push_back(L' ');
            }
            if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
                line.append(argument);
                return;
            }
            line.push_back(L'"');
            std::size_t backslashes = 0;
            for (const wchar_t c: argument) {
                if (c == L'\\') {
                    ++backslashes;
                    continue;
                }
                // Backslashes before a quote are doubled, and the quote escaped; others stay as they are.
                line.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
                backslashes = 0;
                line.push_back(c);
            }
            line.append(backslashes * 2, L'\\');
            line.push_back(L'"');
        }

        // The editor's environment with the request's variables set over it (names compared
        // without case, as Windows does), sorted like Windows keeps it, as CreateProcessW takes it.
        std::wstring environmentBlock(const std::vector<std::pair<std::string, std::string>>& overrides) {
            std::vector<std::wstring> entries;
            std::vector<std::wstring> names;
            names.reserve(overrides.size());
            for (const auto& [key, value]: overrides) {
                names.push_back(widen(key));
            }
            const auto sameName = [](const std::wstring& a, const std::wstring& b) {
                return ::CompareStringOrdinal(a.data(), int(a.size()), b.data(), int(b.size()), TRUE) == CSTR_EQUAL;
            };
            if (wchar_t* block = ::GetEnvironmentStringsW()) {
                for (const wchar_t* entry = block; *entry; entry += std::wcslen(entry) + 1) {
                    const std::wstring_view text(entry);
                    // "=C:=C:\..." entries keep the current folder of each drive; their name starts after the first '='.
                    const auto equals = text.find(L'=', 1);
                    const std::wstring name(text.substr(0, equals));
                    if (std::none_of(names.begin(), names.end(), [&](const std::wstring& other) {
                            return sameName(name, other);
                        })) {
                        entries.emplace_back(text);
                    }
                }
                ::FreeEnvironmentStringsW(block);
            }
            for (std::size_t i = 0; i < overrides.size(); ++i) {
                entries.push_back(names[i] + L"=" + widen(overrides[i].second));
            }
            std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
                return ::CompareStringOrdinal(a.data(), int(a.size()), b.data(), int(b.size()), TRUE) == CSTR_LESS_THAN;
            });
            std::wstring result;
            for (const auto& entry: entries) {
                result.append(entry).push_back(L'\0');
            }
            result.push_back(L'\0');
            return result;
        }

        // A started game: its process and the Job object that holds it and every process it starts.
        struct Started {
            HANDLE process = nullptr;
            HANDLE job = nullptr;  // null when the editor's own job does not allow one
            bool stopAsked = false;
        };
        std::map<std::int64_t, Started>& startedGames() {
            static std::map<std::int64_t, Started> games;
            return games;
        }

        void endAll(Started& started, UINT code) {
            if (started.job) {
                ::TerminateJobObject(started.job, code);
            } else {
                ::TerminateProcess(started.process, code);
            }
        }

        // The ids of the processes in the job (or just the game's).
        std::vector<DWORD> processIds(const Started& started, std::int64_t pid) {
            std::vector<DWORD> ids;
            if (started.job) {
                // Room for many: a list that does not fit is cut, not refused (ERROR_MORE_DATA).
                constexpr DWORD capacity = 256;
                std::vector<std::byte> buffer(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + capacity * sizeof(ULONG_PTR));
                auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
                list->NumberOfAssignedProcesses = capacity;
                if (::QueryInformationJobObject(started.job,
                                                JobObjectBasicProcessIdList,
                                                list,
                                                DWORD(buffer.size()),
                                                nullptr) ||
                    ::GetLastError() == ERROR_MORE_DATA) {
                    for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i) {
                        ids.push_back(DWORD(list->ProcessIdList[i]));
                    }
                }
            }
            if (ids.empty()) {
                ids.push_back(DWORD(pid));
            }
            return ids;
        }

        struct CloseRequest {
            const std::vector<DWORD>* ids = nullptr;
        };

        BOOL CALLBACK closeWindow(HWND window, LPARAM parameter) {
            const auto& request = *reinterpret_cast<const CloseRequest*>(parameter);
            DWORD owner = 0;
            ::GetWindowThreadProcessId(window, &owner);
            if (std::find(request.ids->begin(), request.ids->end(), owner) != request.ids->end()) {
                ::PostMessageW(window, WM_CLOSE, 0, 0);
            }
            return TRUE;
        }
    }

    std::string homeFolder() {
        wchar_t buffer[MAX_PATH * 2];
        const DWORD size = ::GetEnvironmentVariableW(L"USERPROFILE", buffer, DWORD(std::size(buffer)));
        return size > 0 && size < std::size(buffer) ? narrow(buffer, int(size)) : std::string();
    }

    Launched launchProcess(const LaunchRequest& request) {
        const auto program = widen(request.program);
        std::wstring commandLine;
        appendArgument(commandLine, program);
        for (const auto& argument: request.arguments) {
            appendArgument(commandLine, widen(argument));
        }
        auto environment = environmentBlock(request.environment);

        SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        const auto log = request.logPath.empty() ? std::wstring(L"NUL") : widen(request.logPath);
        HANDLE output = ::CreateFileW(log.c_str(),
                                      GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      &inheritable,
                                      CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
        HANDLE input = ::CreateFileW(L"NUL",
                                     GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &inheritable,
                                     OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL,
                                     nullptr);
        if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
            const auto error = lastError();
            if (output != INVALID_HANDLE_VALUE) {
                ::CloseHandle(output);
            }
            if (input != INVALID_HANDLE_VALUE) {
                ::CloseHandle(input);
            }
            return Launched{0, "cannot open " + request.logPath + ": " + error};
        }

        // Only the log and NUL are inherited, not every inheritable handle of the editor.
        HANDLE inherited[] = {input, output};
        SIZE_T attributeSize = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        std::vector<std::byte> attributeBuffer(attributeSize);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
        ::InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize);
        ::UpdateProcThreadAttribute(attributes,
                                    0,
                                    PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                    inherited,
                                    sizeof(inherited),
                                    nullptr,
                                    nullptr);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input;
        startup.StartupInfo.hStdOutput = output;
        startup.StartupInfo.hStdError = output;
        startup.lpAttributeList = attributes;

        // The game's own folder is its working folder (GeneralsX finds its DLLs and files there).
        const auto folder = std::filesystem::path(program).parent_path().wstring();
        PROCESS_INFORMATION info{};
        // Suspended until it is in its Job object, so nothing it starts escapes it. A console
        // program gets no console window; the game's own window is not affected.
        const DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP |
                            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
        const BOOL created = ::CreateProcessW(program.c_str(),
                                              commandLine.data(),
                                              nullptr,
                                              nullptr,
                                              TRUE,
                                              flags,
                                              environment.data(),
                                              folder.empty() ? nullptr : folder.c_str(),
                                              &startup.StartupInfo,
                                              &info);
        const auto error = created ? std::string() : lastError();
        ::DeleteProcThreadAttributeList(attributes);
        ::CloseHandle(input);
        ::CloseHandle(output);
        if (!created) {
            return Launched{0, "cannot start " + request.program + ": " + error};
        }
        // No JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: the game keeps running when the editor quits.
        HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
        if (job && !::AssignProcessToJobObject(job, info.hProcess)) {
            ::CloseHandle(job);
            job = nullptr;
        }
        ::ResumeThread(info.hThread);
        ::CloseHandle(info.hThread);
        const auto pid = std::int64_t(info.dwProcessId);
        startedGames()[pid] = Started{info.hProcess, job, false};
        return Launched{pid, ""};
    }

    std::optional<int> pollProcess(std::int64_t pid) {
        auto& games = startedGames();
        const auto found = games.find(pid);
        if (pid <= 0 || found == games.end()) {
            return 0;  // not one this editor started (or already reported), like ECHILD
        }
        auto& started = found->second;
        if (::WaitForSingleObject(started.process, 0) == WAIT_TIMEOUT) {
            return std::nullopt;
        }
        DWORD code = 0;
        if (!::GetExitCodeProcess(started.process, &code)) {
            code = DWORD(-1);
        }
        // Asked to stop: what the game left running goes with it (the whole game, as on macOS).
        if (started.stopAsked && started.job) {
            ::TerminateJobObject(started.job, UINT(killedExitCode));
        }
        ::CloseHandle(started.process);
        if (started.job) {
            ::CloseHandle(started.job);
        }
        games.erase(found);
        return int(code);
    }

    void stopProcess(std::int64_t pid, bool force) {
        auto& games = startedGames();
        const auto found = games.find(pid);
        if (pid <= 0 || found == games.end()) {
            return;
        }
        auto& started = found->second;
        started.stopAsked = true;
        if (force) {
            endAll(started, UINT(killedExitCode));
            return;
        }
        // Like closing its window: the game quits by itself (a process without a window does not
        // hear it, and is ended by force when it does not quit in time).
        const auto ids = processIds(started, pid);
        CloseRequest request{&ids};
        ::EnumWindows(closeWindow, reinterpret_cast<LPARAM>(&request));
    }

    bool processRuns(std::int64_t pid) {
        if (pid <= 0) {
            return false;
        }
        HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
        if (!process) {
            return false;
        }
        const bool runs = ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        ::CloseHandle(process);
        return runs;
    }
#else
    Launched launchProcess(const LaunchRequest& request) {
        std::vector<std::string> environment;
        for (char** entry = environ; entry && *entry; ++entry) {
            const std::string text(*entry);
            const auto name = text.substr(0, text.find('='));
            bool replaced = false;
            for (const auto& [key, value]: request.environment) {
                replaced = replaced || key == name;
            }
            if (!replaced) {
                environment.push_back(text);
            }
        }
        for (const auto& [key, value]: request.environment) {
            environment.push_back(key + "=" + value);
        }
        std::vector<char*> envp;
        for (auto& entry: environment) {
            envp.push_back(entry.data());
        }
        envp.push_back(nullptr);

        std::vector<std::string> arguments{request.program};
        arguments.insert(arguments.end(), request.arguments.begin(), request.arguments.end());
        std::vector<char*> argv;
        for (auto& argument: arguments) {
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
        if (result != 0) {
            return Launched{0, "cannot start " + request.program + ": " + std::strerror(result)};
        }
        return Launched{pid, ""};
    }

    std::optional<int> pollProcess(std::int64_t pid) {
        if (pid <= 0) {
            return 0;
        }
        int status = 0;
        const auto done = ::waitpid(pid_t(pid), &status, WNOHANG);
        if (done == 0) {
            return std::nullopt;
        }
        if (done < 0) {
            return errno == ECHILD ? 0 : -1;
        }
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    }

    void stopProcess(std::int64_t pid, bool force) {
        if (pid > 0) {
            // The whole process group: it is the leader of its own session (POSIX_SPAWN_SETSID), and
            // a launcher script may run the game as its child (GeneralsX's run.sh pipes its output).
            const int signal = force ? SIGKILL : SIGTERM;
            if (::kill(-pid_t(pid), signal) != 0) {
                ::kill(pid_t(pid), signal);
            }
        }
    }

    bool processRuns(std::int64_t pid) {
        return pid > 0 && ::kill(pid_t(pid), 0) == 0;
    }
#endif
}
