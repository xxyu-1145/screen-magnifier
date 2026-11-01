// app/main.cpp — process entry point.
//
// Kept deliberately thin: everything the app does lives in AppHost, so the
// entry point only has to establish process-wide state (DPI awareness and
// common controls) and surface a startup failure to the user.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <dbghelp.h>

#include <string>

#include "app/app_host.h"
#include "platform/topology.h"

namespace {

void report_startup_failure(const std::string& message) {
    const std::string text = "Screen Magnifier could not start.\n\n" + message;
    ::MessageBoxA(nullptr, text.c_str(), "Screen Magnifier", MB_OK | MB_ICONERROR);
}

// Diagnostics only: with MAG_CRASH_DUMP set to a file path, an unhandled
// exception writes a minidump there before the process dies, so a crash on a
// machine without a debugger still leaves a stack behind. Normal runs are
// untouched -- the environment variable is never set outside the harnesses.
LONG WINAPI write_crash_dump(EXCEPTION_POINTERS* info) {
    wchar_t path[MAX_PATH]{};
    if (::GetEnvironmentVariableW(L"MAG_CRASH_DUMP", path, MAX_PATH) == 0 ||
        path[0] == L'\0') {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const HANDLE file = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = ::GetCurrentThreadId();
    mei.ExceptionPointers = info;
    mei.ClientPointers = FALSE;
    ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), file,
                        static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory),
                        info ? &mei : nullptr, nullptr, nullptr);
    ::CloseHandle(file);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

// MinGW's crtexewin startup looks for WinMain rather than wWinMain, so the
// ANSI entry point is used and every Win32 call inside the app is the explicit
// ...W form.
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    ::SetUnhandledExceptionFilter(&write_crash_dump);

    // The embedded manifest already declares PerMonitorV2 and the comctl32 v6
    // dependency, but this costs nothing and covers a build that was not
    // embedded -- the console variant, or one linked by hand.
    mag::enable_per_monitor_dpi_v2();

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS;
    ::InitCommonControlsEx(&icc);

    // A second instance would fight over the same global hotkeys.
    HANDLE single = ::CreateMutexW(nullptr, TRUE, L"Local\\ScreenMagnifier.SingleInstance");
    if (single && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::MessageBoxW(nullptr,
                      L"Screen Magnifier is already running.\n"
                      L"Look for its icon in the notification area.",
                      L"Screen Magnifier", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    mag::AppHost host(instance);
    if (!host.initialize()) {
        report_startup_failure(host.last_error());
        return 1;
    }

    const int exit_code = host.run();
    host.shutdown();
    if (single) ::CloseHandle(single);
    return exit_code;
}
