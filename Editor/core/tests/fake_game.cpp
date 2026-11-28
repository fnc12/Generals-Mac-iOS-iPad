// A stand-in for the game app in the Windows Play tests (map_flow_tests.cpp), what Fake.app's run.sh
// is on macOS and Linux. Copied next to the test's files as Fake.exe, it reads fake.cfg there
// ("<seconds> <exit code> <ignore close 0|1>"), writes the arguments it got (args.txt, one per line),
// CNC_GENERALS_ZH_PATH, CNC_GENERALS_PATH and GENERALSX_USER_DATA_DIR (env.txt) and appends
// "started <pid>" to runs.txt; then it runs a child (itself with --child <seconds>, its id in
// child.txt), like GeneralsX's launcher runs the game, and exits with the code once the child ends.
// Its hidden window quits it with exit code 3 when closed (WM_CLOSE, what play.stop sends), unless
// it ignores that.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {
    std::string narrow(const std::wstring& text) {
        if (text.empty()) {
            return {};
        }
        const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
        std::string result(std::size_t(size), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), result.data(), size, nullptr, nullptr);
        return result;
    }

    std::string variable(const wchar_t* name) {
        wchar_t buffer[4096];
        const DWORD size = ::GetEnvironmentVariableW(name, buffer, DWORD(std::size(buffer)));
        return size > 0 && size < std::size(buffer) ? narrow(std::wstring(buffer, size)) : std::string();
    }

    constexpr int closedExitCode = 3;
    bool ignoreClose = false;

    LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_CLOSE) {
            if (!ignoreClose) {
                ::PostQuitMessage(closedExitCode);
            }
            return 0;
        }
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int count = 0;
    wchar_t** arguments = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (count == 3 && std::wstring(arguments[1]) == L"--child") {
        ::Sleep(DWORD(std::stoi(arguments[2])) * 1000);
        return 0;
    }
    wchar_t self[MAX_PATH];
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    const auto folder = std::filesystem::path(self).parent_path();
    int seconds = 0, code = 0, ignore = 0;
    std::ifstream(folder / "fake.cfg") >> seconds >> code >> ignore;
    ignoreClose = ignore != 0;
    {
        std::ofstream file(folder / "args.txt", std::ios::binary);
        for (int i = 1; i < count; ++i) {
            file << narrow(arguments[i]) << '\n';
        }
    }
    std::ofstream(folder / "env.txt", std::ios::binary) << variable(L"CNC_GENERALS_ZH_PATH") << '\n'
                                                        << variable(L"CNC_GENERALS_PATH") << '\n'
                                                        << variable(L"GENERALSX_USER_DATA_DIR") << '\n';
    std::ofstream(folder / "runs.txt", std::ios::binary | std::ios::app)
        << "started " << ::GetCurrentProcessId() << '\n';

    std::wstring childLine = L"\"" + std::wstring(self) + L"\" --child " + std::to_wstring(seconds);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!::CreateProcessW(self, childLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child)) {
        return 100;
    }
    ::CloseHandle(child.hThread);
    std::ofstream(folder / "child.txt", std::ios::binary) << child.dwProcessId;

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"EndEditorFakeGame";
    ::RegisterClassW(&windowClass);
    ::CreateWindowExW(0,
                      windowClass.lpszClassName,
                      L"Fake game",
                      WS_OVERLAPPEDWINDOW,
                      0,
                      0,
                      100,
                      100,
                      nullptr,
                      nullptr,
                      instance,
                      nullptr);

    for (;;) {
        const DWORD woke = ::MsgWaitForMultipleObjects(1, &child.hProcess, FALSE, INFINITE, QS_ALLINPUT);
        if (woke == WAIT_OBJECT_0) {
            return code;  // the child ended: the game is over
        }
        MSG message;
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                return int(message.wParam);
            }
            ::DispatchMessageW(&message);
        }
    }
}
