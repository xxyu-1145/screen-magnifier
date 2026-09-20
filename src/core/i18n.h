// core/i18n.h — user-facing text.
//
// Every string is declared once, in one table, with its Chinese and English
// forms side by side. The enumeration and the lookup table are both generated
// from that same list, so a translation cannot be added to one and forgotten in
// the other, and no entry can silently drift out of order.
//
// core/ must stay free of <windows.h>, so the UTF-8 to UTF-16 conversion this
// needs is implemented here rather than borrowed from the platform layer.
#pragma once

#include <cstdint>
#include <string>

#include "core/events.h"

namespace mag {

enum class Language : std::uint32_t { Chinese = 0, English = 1 };

// name, 简体中文, English
#define MAG_STRING_TABLE(X)                                                              \
    X(AppTitle, "屏幕放大镜", "Screen Magnifier")                                        \
    X(LanguageSection, "语言", "Language")                                               \
    X(LanguageChinese, "中文", "中文")                                                   \
    X(LanguageEnglish, "English", "English")                                             \
    /* --- sections --- */                                                               \
    X(SectionZoom, "放大倍率", "Magnification")                                          \
    X(SectionShape, "选区形状", "Selection shape")                                       \
    X(SectionWindow, "放大窗口", "Output window")                                        \
    X(SectionHotkeys, "快捷键", "Hotkeys")                                               \
    X(SectionHotkeysView, "快捷键 · 显示与倍率", "Hotkeys · display and zoom")            \
    X(SectionHotkeysWindow, "快捷键 · 窗口与程序", "Hotkeys · window and app")            \
    X(SectionInteraction, "交互", "Interaction")                                         \
    X(SectionRendering, "渲染", "Rendering")                                             \
    X(SectionActions, "操作", "Actions")                                                 \
    X(SectionStatus, "状态", "Status")                                                   \
    /* --- shapes --- */                                                                 \
    X(ShapeRectangle, "矩形", "Rectangle")                                               \
    X(ShapeCircle, "圆形", "Circle")                                                     \
    X(ShapeEllipse, "椭圆", "Ellipse")                                                   \
    X(ShapeRounded, "圆角矩形", "Rounded")                                               \
    /* --- zoom / output window --- */                                                   \
    X(ZoomFactor, "倍率", "Factor")                                                      \
    X(ZoomFit, "适配选区", "Fit to source")                                              \
    X(Width, "宽", "Width")                                                              \
    X(Height, "高", "Height")                                                            \
    X(Apply, "应用", "Apply")                                                            \
    X(KeepAspect, "保持比例", "Keep aspect")                                       \
    X(SaveSelection, "保存选区", "Save region")                                    \
    X(SelectionSaved, "已保存到选区 ", "saved to region ")                          \
    X(SelectionSavedMark, "已保存 ✓", "Saved ✓")                                     \
    /* --- hotkey actions --- */                                                         \
    X(HkToggleMagnifier, "显示 / 隐藏放大镜", "Show / hide")              \
    X(HkTogglePassThrough, "切换鼠标穿透", "Click-through")                       \
    X(HkCycleShape, "切换选区形状", "Cycle shape")                             \
    X(HkZoomIn, "放大", "Zoom in")                                                       \
    X(HkZoomOut, "缩小", "Zoom out")                                                     \
    X(HkPreset1, "倍率预设 1", "Preset 1")                                               \
    X(HkPreset2, "倍率预设 2", "Preset 2")                                               \
    X(HkPreset3, "倍率预设 3", "Preset 3")                                               \
    X(HkPreset4, "倍率预设 4", "Preset 4")                                               \
    X(HkGrowWidth, "加宽放大窗口", "Wider")                            \
    X(HkShrinkWidth, "收窄放大窗口", "Narrower")                         \
    X(HkGrowHeight, "增高放大窗口", "Taller")                        \
    X(HkShrinkHeight, "降低放大窗口", "Shorter")                        \
    X(HkResetSelection, "重置选区", "Reset selection")                               \
    X(HkCenterOutput, "放大窗口回到屏幕中央", "Centre the window")                     \
    X(HkQuit, "退出程序", "Quit")                                                        \
    X(PressAKey, "请按新的组合键…", "Press a key…")                                      \
    X(Ok, "确定", "OK")                                                                  \
    X(Cancel, "取消", "Cancel")                                                          \
    X(Unbound, "未设置", "unbound")                                                      \
    X(HotkeyNeedsModifier, "需要配合 Ctrl / Alt / Shift / Win",                          \
      "needs Ctrl, Alt, Shift or Win")                                                   \
    X(HotkeyHint, "点击右侧输入框，然后按下新的组合键；Esc 取消",                         \
      "Click a field, then press the new chord; Esc cancels")                              \
    /* --- interaction --- */                                                            \
    X(PassThroughNow, "立即鼠标穿透", "Pass-through now")                                \
    X(StrictCompat, "严格兼容模式", "Strict compatibility mode")                         \
    X(EdgeDwell, "边缘唤回", "Edge dwell")                                               \
    X(EdgeDwellUnit, "毫秒后窗口恢复可操作", "ms until operable again")   \
    /* --- rendering --- */                                                              \
    X(Filter, "缩放算法", "Filter")                                                      \
    X(FilterAuto, "自动", "Auto")                                                        \
    X(FilterPoint, "邻近", "Point")                                                      \
    X(FilterBilinear, "双线性", "Bilinear")                                              \
    X(FilterBicubic, "双三次", "Bicubic")                                                \
    X(ExcludeCapture, "捕获时排除自身", "Exclude from capture")                          \
    X(ShowBorder, "显示边框", "Show border")                                             \
    /* --- actions --- */                                                                \
    X(PickRegion, "框选区域…", "Pick region...")                                          \
    X(StartMagnifier, "开启放大", "Start magnifier")                                     \
    X(StopMagnifier, "停止放大", "Stop magnifier")                                       \
    X(SaveSettings, "保存设置", "Save settings")                                         \
    X(RestoreDefaults, "恢复默认", "Restore defaults")                                   \
    X(RestoreConfirm, "所有设置已恢复为默认值", "all settings restored to their defaults")  \
    X(Quit, "退出", "Quit")                                                              \
    /* --- status --- */                                                                 \
    X(StatusSelection, "选区", "Selection")                                              \
    X(StatusOutput, "输出", "Output")                                                    \
    X(StatusCapture, "捕获", "Capture")                                                  \
    X(StatusRenderer, "渲染器", "Renderer")                                              \
    X(StateOff, "已关闭", "off")                                                         \
    X(StatusOffHint, "（按快捷键，或点下方「开启放大」）",                               \
                     " (use the hotkey, or Start below)")                                \
    X(StateInteractive, "可操作", "interactive")                                         \
    X(StatePassThrough, "鼠标穿透", "click-through")                                     \
    X(StateEdgeArmed, "边缘唤回", "armed")                                               \
    X(StateSuspended, "已暂停", "suspended")                                             \
    X(CaptureDxgi, "DXGI 桌面复制", "DXGI Desktop Duplication")                          \
    X(CaptureGdi, "GDI 兼容模式", "GDI fallback")                                        \
    X(CaptureFallbackNote, "硬件捕获不可用，已切换到兼容模式",                            \
      "hardware capture unavailable, using the fallback")                                \
    X(DisplayChanged, "显示配置已更改", "display configuration changed")                 \
    X(CaptureLost, "捕获丢失，正在尝试恢复", "capture lost, attempting to recover")      \
    X(RendererUnavailable, "渲染器不可用：", "renderer unavailable: ")                   \
    X(SettingsRestored, "配置文件无法使用，已恢复默认", "config unusable, defaults restored") \
    X(HotkeyConflict, "部分快捷键已被其他程序占用", "some hotkeys are owned by another app") \
    X(HotkeyTakenOver, "部分快捷键原被其他程序占用，已由本程序接管",                            \
                       "some hotkeys were owned by another app; taken over")              \
    X(HotkeyMouseHooked, "鼠标按键的快捷键由低级钩子生效",                                  \
                         "mouse-button hotkeys are served by the low-level hook")          \
    X(HotkeyMouseRefused, "严格兼容模式下鼠标按键快捷键不可用",                              \
                          "mouse buttons need the low-level hook, which strict mode refuses") \
    X(HotkeyConflictCount, "个快捷键已被其他程序占用", " hotkey(s) are owned by another app") \
    X(SingleInstance, "屏幕放大镜已在运行", "Screen Magnifier is already running")       \
    X(SingleInstanceHint, "请查看任务栏通知区域中的图标。",                            \
      "Look for its icon in the notification area.")                                     \
    X(StartupFailed, "屏幕放大镜无法启动。", "Screen Magnifier could not start.")        \
    X(ExcludeRefused, "系统拒绝将窗口排除出捕获，可能会自镜像",                            \
      "the OS refused capture exclusion; the window may mirror itself")                  \
    /* --- tray --- */                                                                   \
    X(TrayTooltip, "屏幕放大镜", "Screen Magnifier")                                     \
    X(TrayShowHide, "显示 / 隐藏放大镜", "Show or hide magnifier")                       \
    X(TrayPickRegion, "框选区域…", "Pick region...")                                     \
    X(TrayClickThrough, "切换鼠标穿透", "Toggle click-through")                          \
    X(TrayCycleShape, "切换选区形状", "Cycle shape")                                     \
    X(TraySettings, "设置…", "Settings...")                                              \
    X(TrayReloadConfig, "重新载入配置", "Reload config")                                 \
    X(TrayQuit, "退出", "Quit")                                                          \
    X(TrayStarted, "已在后台运行，按 Ctrl+Alt+M 开启放大",                                \
      "Running in the background; press Ctrl+Alt+M to magnify")                          \
    /* --- picker --- */                                                                 \
    X(PickerHint, "拖动框选 · 方向键微调 · 1-4 或 Tab 换形状 · Enter 确认 · Esc 取消",    \
      "Drag to select · arrows nudge · 1-4 or Tab for shape · Enter commits · Esc cancels")

enum class Str : int {
#define MAG_STR_ENUM(name, zh, en) name,
    MAG_STRING_TABLE(MAG_STR_ENUM)
#undef MAG_STR_ENUM
    Count,
};

// Localised text. The wide form is cached and stays valid until the language
// changes, which only ever happens on the UI thread.
const wchar_t* tr(Str id) noexcept;
// The same text as UTF-8, for the narrow status strings the app builds.
const char* tr_utf8(Str id) noexcept;

void set_language(Language lang) noexcept;
Language language() noexcept;

// "zh" / "en", and the inverse. Used by the configuration file.
const char* language_code(Language lang) noexcept;
bool language_from_code(const char* code, Language& out) noexcept;

// The localised name of a hotkey action.
Str hotkey_label(HotkeyAction action) noexcept;
// The localised name of an interaction state.
Str state_label(InteractionState state) noexcept;
// The localised name of a selection shape.
Str shape_label(SelectionShape shape) noexcept;

// UTF-8 to UTF-16. Invalid bytes are skipped rather than trapping, so a
// mangled configuration file can never take the UI down.
std::wstring utf8_to_wide(const char* text);
std::wstring utf8_to_wide(const std::string& text);

}  // namespace mag
