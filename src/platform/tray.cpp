// platform/tray.cpp — notification-area icon and its context menu.
//
// The icon is generated at runtime (a 16x16 magnifier rasterised into a DIB
// section) so the executable needs no .ico resource and no resource script.

#include "platform/tray.h"

#include "core/i18n.h"
#include "platform/app_icon.h"

#include <cstddef>
#include <cstring>
#include <iterator>

namespace mag {
namespace {

// The tray is a singleton in this app (app/app_host.h owns one TrayIcon) and
// show_context_menu() is declared without an out-parameter, so the command the
// user picked is left here for handle_message() to read.
unsigned g_menu_command = 0;

// Fixed-size Win32 character arrays take no length, so copying has to be
// bounded by hand; the secure-CRT variants are not portable across toolchains.
void copy_into(wchar_t* destination, std::size_t capacity, const std::wstring& source) noexcept {
    if (capacity == 0) return;
    const std::size_t length = source.size() < capacity - 1 ? source.size() : capacity - 1;
    if (length != 0) std::memcpy(destination, source.data(), length * sizeof(wchar_t));
    destination[length] = L'\0';
}

}  // namespace

TrayIcon::TrayIcon() = default;

TrayIcon::~TrayIcon() {
    destroy();
}

bool TrayIcon::create(HWND owner, HINSTANCE instance, const std::wstring& tooltip) {
    if (owner == nullptr) return false;
    if (created_) {
        set_tooltip(tooltip);
        return true;
    }

    owner_ = owner;
    instance_ = instance;
    tooltip_ = tooltip;

    nid_ = {};
    nid_.cbSize = sizeof(NOTIFYICONDATAW);
    nid_.hWnd = owner_;
    nid_.uID = kIconId;
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid_.uCallbackMessage = kCallbackMessage;
    // The same drawn mark the settings window uses, at the tray's own size.
    nid_.hIcon = make_app_icon(GetSystemMetrics(SM_CXSMICON));
    copy_into(nid_.szTip, std::size(nid_.szTip), tooltip_);

    if (Shell_NotifyIconW(NIM_ADD, &nid_) == FALSE) {
        if (nid_.hIcon != nullptr) {
            DestroyIcon(nid_.hIcon);
            nid_.hIcon = nullptr;
        }
        owner_ = nullptr;
        instance_ = nullptr;
        return false;
    }
    created_ = true;
    visible_ = true;
    return true;
}

void TrayIcon::destroy() noexcept {
    if (created_) {
        if (visible_) Shell_NotifyIconW(NIM_DELETE, &nid_);
        if (nid_.hIcon != nullptr) {
            DestroyIcon(nid_.hIcon);
            nid_.hIcon = nullptr;
        }
        created_ = false;
        visible_ = false;
    }
    owner_ = nullptr;
    instance_ = nullptr;
}

void TrayIcon::set_tooltip(const std::wstring& text) {
    tooltip_ = text;
    copy_into(nid_.szTip, std::size(nid_.szTip), tooltip_);
    if (!created_ || !visible_) return;

    NOTIFYICONDATAW update = nid_;
    update.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &update);
}

void TrayIcon::set_visible(bool visible) {
    if (!created_ || visible == visible_) return;
    if (visible) {
        if (Shell_NotifyIconW(NIM_ADD, &nid_) == FALSE) return;
        visible_ = true;
    } else {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        visible_ = false;
    }
}

void TrayIcon::notify(const std::wstring& title, const std::wstring& text) const {
    if (!created_ || !visible_) return;

    NOTIFYICONDATAW balloon = nid_;
    balloon.uFlags = NIF_INFO;
    balloon.dwInfoFlags = NIIF_INFO;
    copy_into(balloon.szInfoTitle, std::size(balloon.szInfoTitle), title);
    copy_into(balloon.szInfo, std::size(balloon.szInfo), text);
    Shell_NotifyIconW(NIM_MODIFY, &balloon);
}

bool TrayIcon::handle_message(UINT message, WPARAM wparam, LPARAM lparam, unsigned& out_command) {
    out_command = 0;

    if (message == kCallbackMessage) {
        // Version-0 icons put the mouse message straight in lParam and the icon
        // id in wParam; version-4 icons pack LOWORD/HIWORD. Accepting both keeps
        // the menu working whichever protocol the shell negotiated.
        const UINT notify = static_cast<UINT>(LOWORD(static_cast<DWORD_PTR>(lparam)));
        const UINT id_lo = static_cast<UINT>(wparam);
        const UINT id_hi = static_cast<UINT>(HIWORD(static_cast<DWORD_PTR>(lparam)));
        if (id_lo != kIconId && id_hi != kIconId) return false;

        if (notify == WM_RBUTTONUP || notify == WM_CONTEXTMENU) {
            show_context_menu();
            out_command = g_menu_command;
            return true;
        }
        if (notify == WM_LBUTTONDBLCLK) {
            out_command = static_cast<unsigned>(CmdToggle);
            return true;
        }
        return true;  // any other tray mouse message is still ours to consume
    }

    if (message == WM_CONTEXTMENU && created_) {
        // Unlike kCallbackMessage this id is generic, so it only counts as a
        // tray callback while an icon is actually installed.
        show_context_menu();
        out_command = g_menu_command;
        return true;
    }
    return false;
}

void TrayIcon::show_context_menu() {
    g_menu_command = 0;
    if (owner_ == nullptr) return;

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;

    // Built on demand rather than cached, so switching language takes effect
    // the next time the menu is opened.
    AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(CmdToggle), tr(Str::TrayShowHide));
    AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(CmdPickRegion), tr(Str::TrayPickRegion));
    AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(CmdTogglePassThrough),
                tr(Str::TrayClickThrough));
    AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(CmdCycleShape), tr(Str::TrayCycleShape));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(CmdSettings), tr(Str::TraySettings));
    AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(CmdReloadConfig), tr(Str::TrayReloadConfig));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(CmdQuit), tr(Str::TrayQuit));
    SetMenuDefaultItem(menu, static_cast<UINT>(CmdToggle), FALSE);

    POINT anchor{};
    GetCursorPos(&anchor);
    // The foreground rule is what lets the menu dismiss when the user clicks
    // elsewhere; without it a tray menu can stay on screen.
    SetForegroundWindow(owner_);
    const UINT picked = TrackPopupMenu(menu,
                                       TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                       anchor.x, anchor.y, 0, owner_, nullptr);
    PostMessageW(owner_, WM_NULL, 0, 0);
    DestroyMenu(menu);

    g_menu_command = picked;
}

}  // namespace mag
