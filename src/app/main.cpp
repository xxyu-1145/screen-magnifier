// app/main.cpp — process entry point.
//
// Kept deliberately thin: everything the app does lives in AppHost, so the
// entry point only has to establish process-wide state (DPI awareness and
// common controls) and surface a startup failure to the user.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>

#include <string>

#include "app/app_host.h"
#include "platform/topology.h"

namespace {

void report_startup_failure(const std::string& message) {
    const std::string text = "Screen Magnifier could not start.\n\n" + message;
    ::MessageBoxA(nullptr, text.c_str(), "Screen Magnifier", MB_OK | MB_ICONERROR);
}

}  // namespace

// MinGW's crtexewin startup looks for WinMain rather than wWinMain, so the
// ANSI entry point is used and every Win32 call inside the app is the explicit
// ...W form.
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
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
