// core/config.h — persisted user settings and their atomic on-disk store.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "core/events.h"
#include "core/i18n.h"
#include "core/types.h"

namespace mag {

inline constexpr int kPresetCount = 4;
// Saved selections. Four of them, matching the factor presets, because the two
// rows sit next to each other and a different count would read as an oversight.
inline constexpr int kSelectionSlotCount = 4;

// How the source pixels are resampled (design doc §3.2).
enum class ScaleFilter {
    Auto = 0,      // point for integer factors, bilinear otherwise
    Point,         // nearest neighbour; cheapest, crisp pixel edges
    Bilinear,      // balanced default while dragging
    Bicubic,       // smoothest text, most expensive
};

struct AppConfig {
    // --- selection ---
    RectPx selection_bounds_px{0, 0, 320, 240};
    SelectionShape selection_shape{SelectionShape::Rectangle};
    Px selection_corner_radius_px{16};

    // Regions the user has kept. Recalling one is what saves re-dragging a
    // region every time the thing being magnified moves.
    std::array<SelectionConfig, kSelectionSlotCount> selection_slots{};
    // Which slot the selection currently in force came from, or -1 when it came
    // from a free-hand pick. Saving writes over that slot.
    int selection_slot{-1};

    // --- magnification ---
    Q16 factor_q16{kQ16One * 4};  // 4x out of the box
    SizePx output_size_px{640, 480};
    bool keep_aspect_ratio{true};
    std::array<Q16, kPresetCount> presets{
        kQ16One * 2, kQ16One * 4, kQ16One * 8, kQ16One * 10};

    // Output window position in virtual-desktop physical pixels.
    // `output_position_auto` centres the window on the primary screen, which is
    // what a user means by "the middle of the screen".
    bool output_position_auto{true};
    PointPx output_position_px{100, 100};

    // --- interaction ---
    bool start_in_pass_through{false};

    // Edge-dwell recovery: park the cursor this close to the magnifier border
    // for this long and the window becomes operable again (requirement 7).
    bool edge_dwell_enabled{true};
    Px edge_dwell_band_px{8};
    int edge_dwell_ms{500};

    // --- input ---
    // Strict compatibility mode drops the low-level keyboard fallback and
    // keeps only RegisterHotKey (design doc §3.3/§6.5).
    bool strict_compat_mode{false};

    // --- rendering ---
    ScaleFilter scale_filter{ScaleFilter::Auto};
    int target_fps{60};
    // Ask DWM to exclude the magnifier from capture so it cannot mirror itself.
    bool exclude_self_from_capture{true};
    bool show_border{true};

    // --- interface ---
    // Chinese out of the box; switchable at run time from the settings window.
    Language language{Language::Chinese};

    // --- hotkeys ---
    std::array<HotkeyChord, kHotkeyCount> hotkeys{};

    // Fills in the shipped defaults (Ctrl+Alt chords, requirement 4).
    static AppConfig defaults() noexcept;
};

// Validates and repairs a config that came from disk: every field is clamped
// into its documented range so a hand-edited file can never put the app into
// an undefined state. Returns true when the config is usable.
bool validate(AppConfig& cfg, const RectPx& virtual_desktop_px) noexcept;

// Serialises to a small JSON document.
std::string to_json(const AppConfig& cfg);

// Parses JSON, leaving fields absent from the document at their current value.
// Returns false when the text is not usable JSON at all.
bool from_json(const std::string& text, AppConfig& cfg) noexcept;

// Human-readable chord text such as "Ctrl+Alt+M", and the inverse parse.
std::string describe_chord(const HotkeyChord& chord);
bool parse_chord(const std::string& text, HotkeyChord& out) noexcept;

// Maps a virtual key code to a stable display name.
std::string describe_virtual_key(std::uint32_t vk);

// ---------------------------------------------------------------------------
// ConfigStore — atomic persistence on a background thread.
// ---------------------------------------------------------------------------

class ConfigStore {
public:
    ConfigStore();
    ~ConfigStore();

    ConfigStore(const ConfigStore&) = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    // Resolves %APPDATA%\Magnifier\config.json (creating the directory).
    static std::string default_path();

    // Blocking load; used once at startup before any thread starts.
    // Missing file yields `fallback` and returns true.
    bool load_now(const std::string& path, AppConfig& out, const AppConfig& fallback);

    // Queues a save. Coalesces bursts: only the newest config is written.
    void save_async(const AppConfig& cfg);

    // Drains any pending write and joins the worker.
    void flush();

    const std::string& last_error() const noexcept { return last_error_; }

private:
    struct Impl;
    Impl* impl_;
    std::string last_error_;
};

}  // namespace mag
