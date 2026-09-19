// platform/tray.h — notification-area icon and its context menu.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <functional>
#include <string>

namespace mag {

class TrayIcon {
public:
    // Menu command ids, also delivered through the owner window's WM_COMMAND.
    enum Command : unsigned {
        CmdToggle = 1001,
        CmdPickRegion,
        CmdTogglePassThrough,
        CmdCycleShape,
        CmdSettings,
        CmdReloadConfig,
        CmdQuit,
    };

    TrayIcon();
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // Adds the icon. Returns false when Shell_NotifyIcon refuses.
    bool create(HWND owner, HINSTANCE instance, const std::wstring& tooltip);

    void destroy() noexcept;

    void set_tooltip(const std::wstring& text);
    void set_visible(bool visible);

    // Shows a balloon notification, e.g. when capture becomes unavailable.
    void notify(const std::wstring& title, const std::wstring& text) const;

    // Call from the owner's window procedure. Returns true when the message
    // was a tray callback (in which case *out_command may be set).
    bool handle_message(UINT message, WPARAM wparam, LPARAM lparam, unsigned& out_command);

    static constexpr UINT kCallbackMessage = WM_APP + 42;
    static constexpr UINT kIconId = 1;

private:
    void show_context_menu();

    HWND owner_{nullptr};
    HINSTANCE instance_{nullptr};
    NOTIFYICONDATAW nid_{};
    bool created_{false};
    bool visible_{false};
    std::wstring tooltip_;
};

}  // namespace mag
