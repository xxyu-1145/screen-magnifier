// core/i18n.cpp — the string tables and the UTF-8/UTF-16 bridge.
//
// Contract: set_language() and the tr()/tr_utf8() lookups are used from the UI
// thread only. The wide forms live in a fixed array that is rebuilt in place
// when the language changes, so the pointers stay valid for as long as nobody
// switches language concurrently with a repaint.

#include "core/i18n.h"

#include <array>

namespace mag {
namespace {

// name, 简体中文, English
const char* const kTable[][2] = {
#define MAG_STR_ROW(name, zh, en) {zh, en},
    MAG_STRING_TABLE(MAG_STR_ROW)
#undef MAG_STR_ROW
};

constexpr std::size_t kStringCount = static_cast<std::size_t>(Str::Count);
static_assert(sizeof(kTable) / sizeof(kTable[0]) == kStringCount,
              "every Str enumerator needs exactly one table row");

Language g_language = Language::Chinese;
std::array<std::wstring, kStringCount> g_wide;

void rebuild_wide() noexcept {
    const int column = static_cast<int>(g_language);
    for (std::size_t i = 0; i < kStringCount; ++i) {
        g_wide[i] = utf8_to_wide(kTable[i][column]);
    }
}

struct WideInit {
    WideInit() { rebuild_wide(); }
};
const WideInit g_init;

}  // namespace

std::wstring utf8_to_wide(const char* text) {
    std::wstring out;
    if (!text) return out;

    const auto* p = reinterpret_cast<const unsigned char*>(text);
    while (*p != 0) {
        unsigned int cp = 0;
        int continuation = 0;
        if (*p < 0x80) {
            cp = *p;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = *p & 0x1Fu;
            continuation = 1;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = *p & 0x0Fu;
            continuation = 2;
        } else if ((*p & 0xF8) == 0xF0) {
            cp = *p & 0x07u;
            continuation = 3;
        } else {
            ++p;  // stray continuation byte or an over-long form: skip it
            continue;
        }
        ++p;
        for (int i = 0; i < continuation && (*p & 0xC0) == 0x80; ++i, ++p) {
            cp = (cp << 6) | (*p & 0x3Fu);
        }

        if (cp <= 0xFFFF) {
            out.push_back(static_cast<wchar_t>(cp));
        } else {
            cp -= 0x10000u;
            out.push_back(static_cast<wchar_t>(0xD800u + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00u + (cp & 0x3FFu)));
        }
    }
    return out;
}

std::wstring utf8_to_wide(const std::string& text) {
    return utf8_to_wide(text.c_str());
}

const wchar_t* tr(Str id) noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index >= kStringCount) return L"";
    return g_wide[index].c_str();
}

const char* tr_utf8(Str id) noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index >= kStringCount) return "";
    return kTable[index][static_cast<int>(g_language)];
}

void set_language(Language lang) noexcept {
    if (lang == g_language) return;
    g_language = lang;
    rebuild_wide();
}

Language language() noexcept {
    return g_language;
}

const char* language_code(Language lang) noexcept {
    return lang == Language::English ? "en" : "zh";
}

bool language_from_code(const char* code, Language& out) noexcept {
    if (!code) return false;
    if (code[0] == 'z' && code[1] == 'h') {
        out = Language::Chinese;
        return true;
    }
    if (code[0] == 'e' && code[1] == 'n') {
        out = Language::English;
        return true;
    }
    return false;
}

Str hotkey_label(HotkeyAction action) noexcept {
    switch (action) {
        case HotkeyAction::ToggleMagnifier: return Str::HkToggleMagnifier;
        case HotkeyAction::TogglePassThrough: return Str::HkTogglePassThrough;
        case HotkeyAction::CycleShape: return Str::HkCycleShape;
        case HotkeyAction::ZoomIn: return Str::HkZoomIn;
        case HotkeyAction::ZoomOut: return Str::HkZoomOut;
        case HotkeyAction::Preset1: return Str::HkPreset1;
        case HotkeyAction::Preset2: return Str::HkPreset2;
        case HotkeyAction::Preset3: return Str::HkPreset3;
        case HotkeyAction::Preset4: return Str::HkPreset4;
        case HotkeyAction::GrowWidth: return Str::HkGrowWidth;
        case HotkeyAction::ShrinkWidth: return Str::HkShrinkWidth;
        case HotkeyAction::GrowHeight: return Str::HkGrowHeight;
        case HotkeyAction::ShrinkHeight: return Str::HkShrinkHeight;
        case HotkeyAction::ResetSelection: return Str::HkResetSelection;
        case HotkeyAction::CenterOutput: return Str::HkCenterOutput;
        case HotkeyAction::Quit: return Str::HkQuit;
        case HotkeyAction::Count: break;
    }
    return Str::HkQuit;
}

Str state_label(InteractionState state) noexcept {
    switch (state) {
        case InteractionState::Off: return Str::StateOff;
        case InteractionState::Interactive: return Str::StateInteractive;
        case InteractionState::PassThrough: return Str::StatePassThrough;
        case InteractionState::EdgeArmed: return Str::StateEdgeArmed;
        case InteractionState::Suspended: return Str::StateSuspended;
    }
    return Str::StateOff;
}

Str shape_label(SelectionShape shape) noexcept {
    switch (shape) {
        case SelectionShape::Rectangle: return Str::ShapeRectangle;
        case SelectionShape::Circle: return Str::ShapeCircle;
        case SelectionShape::Ellipse: return Str::ShapeEllipse;
        case SelectionShape::RoundedRectangle: return Str::ShapeRounded;
    }
    return Str::ShapeRectangle;
}

}  // namespace mag
