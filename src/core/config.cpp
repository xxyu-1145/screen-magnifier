// core/config.cpp — persisted settings, the chord vocabulary and the atomic
// background store (design doc §5.8).
#include "core/config.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/hit_test.h"

namespace mag {
namespace {

// ---------------------------------------------------------------------------
// Chord vocabulary (Win32 constants kept local so core/ stays header-free).
// ---------------------------------------------------------------------------

constexpr std::uint32_t kModAlt = 0x0001;
constexpr std::uint32_t kModControl = 0x0002;
constexpr std::uint32_t kModShift = 0x0004;
constexpr std::uint32_t kModWin = 0x0008;

// Only the codes the shipped chords name; everything else lives in kKeyNames.
constexpr std::uint32_t kVkLeft = 0x25;
constexpr std::uint32_t kVkUp = 0x26;
constexpr std::uint32_t kVkRight = 0x27;
constexpr std::uint32_t kVkDown = 0x28;
constexpr std::uint32_t kVkOemPlus = 0xBB;
constexpr std::uint32_t kVkOemMinus = 0xBD;
constexpr std::uint32_t kVkF1 = 0x70;
constexpr std::uint32_t kVkF24 = 0x87;

struct KeyName {
    const char* name;
    std::uint32_t vk;
};

// Canonical display names. describe_virtual_key() emits these and
// parse_chord() accepts them, so the pair always round-trips.
constexpr KeyName kKeyNames[] = {
    {"Backspace", 0x08},        {"Tab", 0x09},          {"Enter", 0x0D},
    {"Pause", 0x13},            {"CapsLock", 0x14},     {"Esc", 0x1B},
    {"Space", 0x20},            {"PageUp", 0x21},       {"PageDown", 0x22},
    {"End", 0x23},              {"Home", 0x24},         {"Left", 0x25},
    {"Up", 0x26},               {"Right", 0x27},        {"Down", 0x28},
    {"PrintScreen", 0x2C},      {"Insert", 0x2D},       {"Delete", 0x2E},
    {"LWin", 0x5B},             {"RWin", 0x5C},         {"Apps", 0x5D},
    {"Num0", 0x60},             {"Num1", 0x61},         {"Num2", 0x62},
    {"Num3", 0x63},             {"Num4", 0x64},         {"Num5", 0x65},
    {"Num6", 0x66},             {"Num7", 0x67},         {"Num8", 0x68},
    {"Num9", 0x69},             {"Num*", 0x6A},         {"Num+", 0x6B},
    {"Num-", 0x6D},             {"Num.", 0x6E},         {"Num/", 0x6F},
    {"NumLock", 0x90},          {"ScrollLock", 0x91},   {"OemSemicolon", 0xBA},
    {"OemPlus", 0xBB},          {"OemComma", 0xBC},     {"OemMinus", 0xBD},
    {"OemPeriod", 0xBE},        {"OemQuestion", 0xBF},  {"OemTilde", 0xC0},
    {"OemOpenBracket", 0xDB},   {"OemBackslash", 0xDC}, {"OemCloseBracket", 0xDD},
    {"OemQuote", 0xDE},
};

// Extra spellings a hand-edited file may use.
constexpr KeyName kKeyAliases[] = {
    {"Escape", 0x1B},   {"Return", 0x0D}, {"Del", 0x2E},   {"Ins", 0x2D},
    {"PgUp", 0x21},     {"PgDn", 0x22},   {"Plus", 0xBB},  {"Minus", 0xBD},
    {"Equal", 0xBB},    {"Equals", 0xBB}, {"Windows", 0x5B}, {"Super", 0x5B},
    {"Meta", 0x5B},     {"Win", 0x5B},    {"Multiply", 0x6A},
};

constexpr std::size_t kKeyNameCount = sizeof(kKeyNames) / sizeof(kKeyNames[0]);
constexpr std::size_t kKeyAliasCount = sizeof(kKeyAliases) / sizeof(kKeyAliases[0]);

char upper_ascii(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

bool is_blank(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool iequals(const std::string& text, const char* other) noexcept {
    const std::size_t n = std::strlen(other);
    if (text.size() != n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (upper_ascii(text[i]) != upper_ascii(other[i])) return false;
    }
    return true;
}

std::string hex_token(std::uint32_t vk) {
    static const char kDigits[] = "0123456789ABCDEF";
    std::string out = "VK_";
    char digits[8];
    int n = 0;
    if (vk == 0) {
        digits[n++] = '0';
    }
    while (vk != 0) {
        digits[n++] = kDigits[vk & 0xFu];
        vk >>= 4;
    }
    while (n > 0) out.push_back(digits[--n]);
    return out;
}

bool vk_from_name(const std::string& name, std::uint32_t& out) noexcept {
    if (name.empty()) return false;

    if (name.size() == 1) {
        const char c = upper_ascii(name[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out = static_cast<std::uint32_t>(c);
            return true;
        }
        if (c == '+') {
            out = kVkOemPlus;
            return true;
        }
        if (c == '-') {
            out = kVkOemMinus;
            return true;
        }
        if (c == '=') {
            out = kVkOemPlus;
            return true;
        }
        return false;
    }

    const char head = upper_ascii(name[0]);
    if (head == 'F') {
        bool digits_only = true;
        int number = 0;
        for (std::size_t i = 1; i < name.size(); ++i) {
            if (name[i] < '0' || name[i] > '9') {
                digits_only = false;
                break;
            }
            number = number * 10 + (name[i] - '0');
        }
        if (digits_only && number >= 1 && number <= 24) {
            out = kVkF1 + static_cast<std::uint32_t>(number - 1);
            return true;
        }
    }
    if (name.size() > 4 && iequals(name.substr(0, 4), "VK_")) {
        std::uint32_t value = 0;
        for (std::size_t i = 4; i < name.size(); ++i) {
            const char c = upper_ascii(name[i]);
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
            value = (value << 4) | digit;
            if (value > 0xFFu) return false;
        }
        out = value;
        return true;
    }

    for (std::size_t i = 0; i < kKeyNameCount; ++i) {
        if (iequals(name, kKeyNames[i].name)) {
            out = kKeyNames[i].vk;
            return true;
        }
    }
    for (std::size_t i = 0; i < kKeyAliasCount; ++i) {
        if (iequals(name, kKeyAliases[i].name)) {
            out = kKeyAliases[i].vk;
            return true;
        }
    }
    return false;
}

bool modifier_bit(const std::string& name, std::uint32_t& out) noexcept {
    if (iequals(name, "Ctrl") || iequals(name, "Control") || iequals(name, "Ctl")) {
        out = kModControl;
        return true;
    }
    if (iequals(name, "Alt") || iequals(name, "Menu")) {
        out = kModAlt;
        return true;
    }
    if (iequals(name, "Shift")) {
        out = kModShift;
        return true;
    }
    if (iequals(name, "Win") || iequals(name, "Windows") || iequals(name, "Super") ||
        iequals(name, "Meta")) {
        out = kModWin;
        return true;
    }
    return false;
}

void bind(AppConfig& cfg, HotkeyAction action, std::uint32_t modifiers,
          std::uint32_t vk) noexcept {
    cfg.hotkeys[static_cast<std::size_t>(action)] = HotkeyChord{modifiers, vk};
}

// ---------------------------------------------------------------------------
// Small integer helpers
// ---------------------------------------------------------------------------

constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kPxLo = std::numeric_limits<Px>::min();
constexpr std::int64_t kPxHi = std::numeric_limits<Px>::max();

constexpr Px add_sat(Px a, Px b) noexcept {
    const std::int64_t r = static_cast<std::int64_t>(a) + static_cast<std::int64_t>(b);
    if (r > kPxHi) return static_cast<Px>(kPxHi);
    if (r < kPxLo) return static_cast<Px>(kPxLo);
    return static_cast<Px>(r);
}

constexpr Px clamp_px(Px v, Px lo, Px hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

constexpr int clamp_int(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

constexpr Q16 clamp_q16(Q16 v, Q16 lo, Q16 hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

constexpr Px larger(Px a, Px b) noexcept { return a > b ? a : b; }

std::int64_t sat_scale(std::int64_t value, std::int64_t factor) noexcept {
    if (value == 0 || factor == 0) return 0;
    if (value > kI64Max / factor) return kI64Max;
    return value * factor;
}

// ---------------------------------------------------------------------------
// Enum <-> text
// ---------------------------------------------------------------------------

const char* filter_name(ScaleFilter f) noexcept {
    switch (f) {
        case ScaleFilter::Auto: return "Auto";
        case ScaleFilter::Point: return "Point";
        case ScaleFilter::Bilinear: return "Bilinear";
        case ScaleFilter::Bicubic: return "Bicubic";
    }
    return "Auto";
}

bool shape_from_text(const std::string& text, SelectionShape& out) noexcept {
    if (iequals(text, "Rectangle") || iequals(text, "Rect")) {
        out = SelectionShape::Rectangle;
        return true;
    }
    if (iequals(text, "Circle")) {
        out = SelectionShape::Circle;
        return true;
    }
    if (iequals(text, "Ellipse")) {
        out = SelectionShape::Ellipse;
        return true;
    }
    if (iequals(text, "RoundedRectangle") || iequals(text, "Rounded")) {
        out = SelectionShape::RoundedRectangle;
        return true;
    }
    return false;
}

bool filter_from_text(const std::string& text, ScaleFilter& out) noexcept {
    if (iequals(text, "Auto")) {
        out = ScaleFilter::Auto;
        return true;
    }
    if (iequals(text, "Point") || iequals(text, "Nearest")) {
        out = ScaleFilter::Point;
        return true;
    }
    if (iequals(text, "Bilinear") || iequals(text, "Linear")) {
        out = ScaleFilter::Bilinear;
        return true;
    }
    if (iequals(text, "Bicubic") || iequals(text, "Cubic")) {
        out = ScaleFilter::Bicubic;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// A minimal JSON document model and reader.
//
// The schema is one flat object, so a tolerant recursive-descent reader is all
// that is needed: unknown keys and mistyped values are skipped rather than
// rejected, and only text that is not JSON at all fails.
// ---------------------------------------------------------------------------

struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind{Kind::Null};
    bool boolean{false};
    std::int64_t number{0};
    std::string text;
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> members;

    const JsonValue* find(const char* key) const noexcept {
        if (kind != Kind::Object) return nullptr;
        for (const auto& entry : members) {
            if (entry.first == key) return &entry.second;
        }
        return nullptr;
    }
};

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

void append_escaped(std::string& out, const std::string& text) {
    static const char kDigits[] = "0123456789ABCDEF";
    out.push_back('"');
    for (char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {
                    out += "\\u00";
                    out.push_back(kDigits[(c >> 4) & 0xFu]);
                    out.push_back(kDigits[c & 0xFu]);
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

class JsonReader {
public:
    explicit JsonReader(const std::string& text) noexcept : text_(text) {}

    bool read_root(JsonValue& out) {
        skip_space();
        if (!read_value(out, 0)) return false;
        skip_space();
        return pos_ == text_.size();
    }

private:
    static constexpr int kMaxDepth = 32;

    bool at(char c) const noexcept { return pos_ < text_.size() && text_[pos_] == c; }

    void skip_space() noexcept {
        while (pos_ < text_.size() && is_blank(text_[pos_])) ++pos_;
    }

    bool read_value(JsonValue& out, int depth) {
        if (depth > kMaxDepth || pos_ >= text_.size()) return false;
        switch (text_[pos_]) {
            case '{': return read_object(out, depth);
            case '[': return read_array(out, depth);
            case '"':
                out.kind = JsonValue::Kind::String;
                return read_string(out.text);
            case 't':
                if (!consume("true")) return false;
                out.kind = JsonValue::Kind::Bool;
                out.boolean = true;
                return true;
            case 'f':
                if (!consume("false")) return false;
                out.kind = JsonValue::Kind::Bool;
                out.boolean = false;
                return true;
            case 'n':
                if (!consume("null")) return false;
                out.kind = JsonValue::Kind::Null;
                return true;
            default: return read_number(out);
        }
    }

    bool consume(const char* word) {
        const std::size_t n = std::strlen(word);
        if (text_.compare(pos_, n, word) != 0) return false;
        pos_ += n;
        return true;
    }

    bool read_array(JsonValue& out, int depth) {
        ++pos_;  // '['
        out.kind = JsonValue::Kind::Array;
        out.items.clear();
        skip_space();
        if (at(']')) {
            ++pos_;
            return true;
        }
        for (;;) {
            JsonValue item;
            skip_space();
            if (!read_value(item, depth + 1)) return false;
            out.items.push_back(std::move(item));
            skip_space();
            if (at(',')) {
                ++pos_;
                continue;
            }
            if (at(']')) {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    bool read_object(JsonValue& out, int depth) {
        ++pos_;  // '{'
        out.kind = JsonValue::Kind::Object;
        out.members.clear();
        skip_space();
        if (at('}')) {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_space();
            std::string key;
            if (!read_string(key)) return false;
            skip_space();
            if (!at(':')) return false;
            ++pos_;
            skip_space();
            JsonValue value;
            if (!read_value(value, depth + 1)) return false;
            out.members.emplace_back(std::move(key), std::move(value));
            skip_space();
            if (at(',')) {
                ++pos_;
                continue;
            }
            if (at('}')) {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    bool read_hex4(std::uint32_t& out) noexcept {
        if (pos_ + 4 > text_.size()) return false;
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = upper_ascii(text_[pos_++]);
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
            value = (value << 4) | digit;
        }
        out = value;
        return true;
    }

    bool read_string(std::string& out) {
        if (!at('"')) return false;
        ++pos_;
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) return false;
            const char escape = text_[pos_++];
            switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!read_hex4(cp)) return false;
                    if (cp >= 0xD800u && cp <= 0xDBFFu && pos_ + 1 < text_.size() &&
                        text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        std::uint32_t low = 0;
                        if (!read_hex4(low)) return false;
                        cp = (low >= 0xDC00u && low <= 0xDFFFu)
                                 ? 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u)
                                 : 0xFFFDu;
                    } else if (cp >= 0xD800u && cp <= 0xDFFFu) {
                        cp = 0xFFFDu;
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;  // unterminated
    }

    bool read_number(JsonValue& out) {
        bool negative = false;
        if (at('-') || at('+')) {
            negative = at('-');
            ++pos_;
        }
        std::int64_t mantissa = 0;
        bool any_digit = false;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            any_digit = true;
            mantissa = sat_scale(mantissa, 10);
            // Guard the add as well: saturating twice must not wrap to a
            // negative number that then reads as a valid field value.
            mantissa = mantissa > kI64Max - 9 ? kI64Max
                                              : mantissa + (text_[pos_] - '0');
            ++pos_;
        }
        if (!any_digit) return false;

        int fractional = 0;
        if (at('.')) {
            ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                // Folding the fraction into the mantissa and dividing it back
                // out again is what truncates a spelling like 2.9 to 2.
                mantissa = sat_scale(mantissa, 10);
                mantissa = mantissa > kI64Max - 9 ? kI64Max
                                                  : mantissa + (text_[pos_] - '0');
                ++pos_;
                ++fractional;
            }
        }
        int exponent = 0;
        if (at('e') || at('E')) {
            ++pos_;
            bool exponent_negative = false;
            if (at('-') || at('+')) {
                exponent_negative = at('-');
                ++pos_;
            }
            bool any_exponent = false;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                any_exponent = true;
                if (exponent < 1000) exponent = exponent * 10 + (text_[pos_] - '0');
                ++pos_;
            }
            if (!any_exponent) return false;
            if (exponent_negative) exponent = -exponent;
        }

        // Every field in this schema is an integer, so a fractional or
        // exponent spelling is tolerated by truncating, never by rounding.
        int shift = exponent - fractional;
        while (shift > 0) {
            mantissa = sat_scale(mantissa, 10);
            --shift;
        }
        while (shift < 0) {
            mantissa /= 10;
            ++shift;
        }

        out.kind = JsonValue::Kind::Number;
        out.number = negative ? -mantissa : mantissa;
        return true;
    }

    const std::string& text_;
    std::size_t pos_{0};
};

bool read_number_field(const JsonValue* value, std::int64_t& out) noexcept {
    if (value == nullptr || value->kind != JsonValue::Kind::Number) return false;
    out = value->number;
    return true;
}

bool read_px_field(const JsonValue* value, Px& out) noexcept {
    std::int64_t n = 0;
    if (!read_number_field(value, n) || n < kPxLo || n > kPxHi) return false;
    out = static_cast<Px>(n);
    return true;
}

bool read_q16_field(const JsonValue* value, Q16& out) noexcept {
    std::int64_t n = 0;
    if (!read_number_field(value, n) || n < 0 || n > 4294967295LL) return false;
    out = static_cast<Q16>(n);
    return true;
}

bool read_int_field(const JsonValue* value, int& out) noexcept {
    std::int64_t n = 0;
    if (!read_number_field(value, n) || n < kPxLo || n > kPxHi) return false;
    out = static_cast<int>(n);
    return true;
}

bool read_bool_field(const JsonValue* value, bool& out) noexcept {
    if (value == nullptr || value->kind != JsonValue::Kind::Bool) return false;
    out = value->boolean;
    return true;
}

bool read_rect_field(const JsonValue* value, RectPx& out) noexcept {
    if (value == nullptr || value->kind != JsonValue::Kind::Array || value->items.size() != 4) {
        return false;
    }
    RectPx rect{};
    if (!read_px_field(&value->items[0], rect.left)) return false;
    if (!read_px_field(&value->items[1], rect.top)) return false;
    if (!read_px_field(&value->items[2], rect.right)) return false;
    if (!read_px_field(&value->items[3], rect.bottom)) return false;
    out = rect;
    return true;
}

bool read_size_field(const JsonValue* value, SizePx& out) noexcept {
    if (value == nullptr || value->kind != JsonValue::Kind::Array || value->items.size() != 2) {
        return false;
    }
    SizePx size{};
    if (!read_px_field(&value->items[0], size.width)) return false;
    if (!read_px_field(&value->items[1], size.height)) return false;
    out = size;
    return true;
}

bool read_point_field(const JsonValue* value, PointPx& out) noexcept {
    if (value == nullptr || value->kind != JsonValue::Kind::Array || value->items.size() != 2) {
        return false;
    }
    PointPx point{};
    if (!read_px_field(&value->items[0], point.x)) return false;
    if (!read_px_field(&value->items[1], point.y)) return false;
    out = point;
    return true;
}

bool read_chord_field(const JsonValue* value, HotkeyChord& out) noexcept {
    if (value == nullptr) return false;
    if (value->kind == JsonValue::Kind::String) {
        HotkeyChord chord{};
        if (!parse_chord(value->text, chord)) return false;
        out = chord;
        return true;
    }
    if (value->kind == JsonValue::Kind::Array && value->items.size() == 2) {
        std::uint32_t modifiers = 0;
        std::uint32_t vk = 0;
        std::int64_t m = 0;
        std::int64_t k = 0;
        if (!read_number_field(&value->items[0], m) || m < 0 || m > 15) return false;
        if (!read_number_field(&value->items[1], k) || k < 0 || k > 255) return false;
        modifiers = static_cast<std::uint32_t>(m);
        vk = static_cast<std::uint32_t>(k);
        out = HotkeyChord{modifiers, vk};
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// JSON writer helpers
// ---------------------------------------------------------------------------

void put_raw(std::string& out, const char* key, const std::string& value) {
    out += "  ";
    append_escaped(out, key);
    out += ": ";
    out += value;
    out += ",\n";
}

void put_num(std::string& out, const char* key, std::int64_t value) {
    put_raw(out, key, std::to_string(value));
}

void put_bool(std::string& out, const char* key, bool value) {
    put_raw(out, key, value ? "true" : "false");
}

void put_text(std::string& out, const char* key, const std::string& value) {
    std::string quoted;
    append_escaped(quoted, value);
    put_raw(out, key, quoted);
}

std::string numbers_to_json(const Px* values, std::size_t count) {
    std::string out = "[";
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) out += ", ";
        out += std::to_string(values[i]);
    }
    out += "]";
    return out;
}

// ---------------------------------------------------------------------------
// The atomic on-disk write
// ---------------------------------------------------------------------------

constexpr std::size_t kMaxConfigBytes = 16u * 1024u * 1024u;

void write_config_file(const std::string& path, const AppConfig& cfg, std::string& error) {
    error.clear();
    const std::filesystem::path target(path);
    std::error_code ec;
    const std::filesystem::path dir = target.parent_path();
    if (!dir.empty()) std::filesystem::create_directories(dir, ec);

    std::filesystem::path temporary = target;
    temporary += ".tmp";

    const std::string text = to_json(cfg);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "cannot open " + temporary.string() + " for writing";
            return;
        }
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream.good()) {
            error = "short write to " + temporary.string();
            return;
        }
    }

    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        // Some volumes refuse a replace-through rename; retry once without the
        // stale target so a save is never silently dropped.
        std::error_code ignored;
        std::filesystem::remove(target, ignored);
        ec.clear();
        std::filesystem::rename(temporary, target, ec);
        if (ec) error = "cannot replace " + path;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// AppConfig
// ---------------------------------------------------------------------------

AppConfig AppConfig::defaults() noexcept {
    AppConfig config;  // member initialisers already carry the shipped values

    constexpr std::uint32_t kCa = kModControl | kModAlt;
    constexpr std::uint32_t kCas = kModControl | kModAlt | kModShift;

    bind(config, HotkeyAction::ToggleMagnifier, kCa, 'M');
    bind(config, HotkeyAction::TogglePassThrough, kCa, 'P');
    bind(config, HotkeyAction::CycleShape, kCa, 'S');
    bind(config, HotkeyAction::ZoomIn, kCa, kVkOemPlus);
    bind(config, HotkeyAction::ZoomOut, kCa, kVkOemMinus);
    bind(config, HotkeyAction::Preset1, kCa, '1');
    bind(config, HotkeyAction::Preset2, kCa, '2');
    bind(config, HotkeyAction::Preset3, kCa, '3');
    bind(config, HotkeyAction::Preset4, kCa, '4');
    bind(config, HotkeyAction::GrowWidth, kCa, kVkRight);
    bind(config, HotkeyAction::ShrinkWidth, kCa, kVkLeft);
    bind(config, HotkeyAction::GrowHeight, kCa, kVkDown);
    bind(config, HotkeyAction::ShrinkHeight, kCa, kVkUp);
    bind(config, HotkeyAction::ResetSelection, kCa, 'R');
    bind(config, HotkeyAction::CenterOutput, kCa, 'C');
    bind(config, HotkeyAction::Quit, kCa, 'Q');
    return config;
}

bool validate(AppConfig& cfg, const RectPx& virtual_desktop_px) noexcept {
    const RectPx desktop = normalize(virtual_desktop_px);
    // Only a degenerate desktop is unusable: the controller cannot place a
    // window at all, so there is nothing sensible to repair into.
    if (is_empty(desktop)) return false;

    const Px max_w = larger(kMinOutputEdgePx, width_of(desktop));
    const Px max_h = larger(kMinOutputEdgePx, height_of(desktop));

    cfg.selection_bounds_px = clamp_into(normalize(cfg.selection_bounds_px), desktop);
    cfg.selection_corner_radius_px =
        clamp_corner_radius(cfg.selection_corner_radius_px, cfg.selection_bounds_px);

    // A slot nobody has filled yet takes the current selection whole -- its
    // shape and corner too -- so recalling one always moves the region somewhere
    // instead of appearing to do nothing. Checking the bounds is what says
    // "never filled": a slot with no rectangle has nothing to recall whatever
    // else it carries.
    for (SelectionConfig& slot : cfg.selection_slots) {
        if (is_empty(normalize(slot.bounds_px))) {
            slot = SelectionConfig{cfg.selection_bounds_px, cfg.selection_shape,
                                   cfg.selection_corner_radius_px};
        }
        slot.bounds_px = clamp_into(normalize(slot.bounds_px), desktop);
        const int shape_value = static_cast<int>(slot.shape);
        if (shape_value < 0 ||
            shape_value > static_cast<int>(SelectionShape::RoundedRectangle)) {
            slot.shape = SelectionShape::Rectangle;
        }
        slot.corner_radius_px = clamp_corner_radius(slot.corner_radius_px, slot.bounds_px);
    }
    if (cfg.selection_slot < -1 || cfg.selection_slot >= kSelectionSlotCount) {
        cfg.selection_slot = -1;
    }
    const int shape = static_cast<int>(cfg.selection_shape);
    if (shape < 0 || shape > static_cast<int>(SelectionShape::RoundedRectangle)) {
        cfg.selection_shape = SelectionShape::Rectangle;
    }

    cfg.factor_q16 = clamp_q16(cfg.factor_q16, kFactorMin, kFactorMax);
    for (Q16& preset : cfg.presets) {
        preset = clamp_q16(preset, kFactorMin, kFactorMax);
    }
    cfg.output_size_px.width = clamp_px(cfg.output_size_px.width, kMinOutputEdgePx, max_w);
    cfg.output_size_px.height = clamp_px(cfg.output_size_px.height, kMinOutputEdgePx, max_h);

    const RectPx wanted{cfg.output_position_px.x, cfg.output_position_px.y,
                        add_sat(cfg.output_position_px.x, cfg.output_size_px.width),
                        add_sat(cfg.output_position_px.y, cfg.output_size_px.height)};
    const RectPx settled = clamp_into(wanted, desktop);
    cfg.output_position_px = PointPx{settled.left, settled.top};

    // The dwell band is a grab margin, never wider than the smallest window.
    cfg.edge_dwell_band_px = clamp_px(cfg.edge_dwell_band_px, 0, kMinOutputEdgePx);
    cfg.edge_dwell_ms = clamp_int(cfg.edge_dwell_ms, 100, 5000);

    const int filter = static_cast<int>(cfg.scale_filter);
    if (filter < 0 || filter > static_cast<int>(ScaleFilter::Bicubic)) {
        cfg.scale_filter = ScaleFilter::Auto;
    }
    cfg.target_fps = clamp_int(cfg.target_fps, 15, 240);
    return true;
}

std::string to_json(const AppConfig& cfg) {
    std::string out;
    out.reserve(1600);
    out += "{\n";
    put_num(out, "version", 1);

    const Px bounds[4] = {cfg.selection_bounds_px.left, cfg.selection_bounds_px.top,
                          cfg.selection_bounds_px.right, cfg.selection_bounds_px.bottom};
    put_raw(out, "selection_bounds_px", numbers_to_json(bounds, 4));
    put_text(out, "selection_shape", to_string(cfg.selection_shape));
    put_num(out, "selection_corner_radius_px", cfg.selection_corner_radius_px);

    std::string slots = "[";
    for (std::size_t i = 0; i < cfg.selection_slots.size(); ++i) {
        if (i != 0) slots += ", ";
        const SelectionConfig& slot = cfg.selection_slots[i];
        const Px box[4] = {slot.bounds_px.left, slot.bounds_px.top, slot.bounds_px.right,
                           slot.bounds_px.bottom};
        slots += "{\"bounds_px\": ";
        slots += numbers_to_json(box, 4);
        slots += ", \"shape\": \"";
        slots += to_string(slot.shape);
        slots += "\", \"corner_radius_px\": ";
        slots += std::to_string(slot.corner_radius_px);
        slots += "}";
    }
    slots += "]";
    put_raw(out, "selection_slots", slots);
    put_num(out, "selection_slot", cfg.selection_slot);

    put_num(out, "factor_q16", cfg.factor_q16);
    const Px output[2] = {cfg.output_size_px.width, cfg.output_size_px.height};
    put_raw(out, "output_size_px", numbers_to_json(output, 2));
    put_bool(out, "keep_aspect_ratio", cfg.keep_aspect_ratio);

    std::string presets = "[";
    for (std::size_t i = 0; i < cfg.presets.size(); ++i) {
        if (i != 0) presets += ", ";
        presets += std::to_string(cfg.presets[i]);
    }
    presets += "]";
    put_raw(out, "presets", presets);

    put_bool(out, "output_position_auto", cfg.output_position_auto);
    const Px position[2] = {cfg.output_position_px.x, cfg.output_position_px.y};
    put_raw(out, "output_position_px", numbers_to_json(position, 2));

    put_bool(out, "start_in_pass_through", cfg.start_in_pass_through);
    put_bool(out, "edge_dwell_enabled", cfg.edge_dwell_enabled);
    put_num(out, "edge_dwell_band_px", cfg.edge_dwell_band_px);
    put_num(out, "edge_dwell_ms", cfg.edge_dwell_ms);

    put_bool(out, "strict_compat_mode", cfg.strict_compat_mode);
    put_text(out, "scale_filter", filter_name(cfg.scale_filter));
    put_num(out, "target_fps", cfg.target_fps);
    put_bool(out, "exclude_self_from_capture", cfg.exclude_self_from_capture);
    put_bool(out, "show_border", cfg.show_border);
    put_text(out, "language", language_code(cfg.language));

    // Last member: it owns the closing brace, so it carries no trailing comma.
    out += "  \"hotkeys\": {";
    std::string body;
    bool first = true;
    for (std::size_t i = 0; i < kHotkeyCount; ++i) {
        const char* name = hotkey_action_name(static_cast<HotkeyAction>(i));
        if (name == nullptr || *name == '\0') continue;
        const HotkeyChord& chord = cfg.hotkeys[i];
        if (!first) body += ",\n";
        first = false;
        body += "    ";
        append_escaped(body, name);
        body += ": [";
        body += std::to_string(chord.modifiers);
        body += ", ";
        body += std::to_string(chord.virtual_key);
        body += "]";
    }
    if (!body.empty()) {
        out += "\n";
        out += body;
        out += "\n  ";
    }
    out += "}\n";
    out += "}\n";
    return out;
}

bool from_json(const std::string& text, AppConfig& cfg) noexcept {
    // Parsing allocates, and the signature is noexcept: a failure to parse is
    // reported as `false`, never as a thrown exception.
    try {
        JsonValue root;
        JsonReader reader(text);
        if (!reader.read_root(root)) return false;
        if (root.kind != JsonValue::Kind::Object) return false;

        read_rect_field(root.find("selection_bounds_px"), cfg.selection_bounds_px);
        if (const JsonValue* value = root.find("selection_shape"); value != nullptr) {
            SelectionShape shape{};
            if (value->kind == JsonValue::Kind::String && shape_from_text(value->text, shape)) {
                cfg.selection_shape = shape;
            } else if (value->kind == JsonValue::Kind::Number) {
                const std::int64_t index = value->number;
                const std::int64_t last =
                    static_cast<std::int64_t>(SelectionShape::RoundedRectangle);
                if (index >= 0 && index <= last) {
                    cfg.selection_shape = static_cast<SelectionShape>(index);
                }
            }
        }
        read_px_field(root.find("selection_corner_radius_px"), cfg.selection_corner_radius_px);

        if (const JsonValue* value = root.find("selection_slots");
            value != nullptr && value->kind == JsonValue::Kind::Array) {
            const std::size_t count = value->items.size() < kSelectionSlotCount
                                          ? value->items.size()
                                          : static_cast<std::size_t>(kSelectionSlotCount);
            for (std::size_t i = 0; i < count; ++i) {
                const JsonValue& item = value->items[i];
                if (item.kind != JsonValue::Kind::Object) continue;
                SelectionConfig& slot = cfg.selection_slots[i];
                read_rect_field(item.find("bounds_px"), slot.bounds_px);
                if (const JsonValue* shape = item.find("shape"); shape != nullptr) {
                    SelectionShape parsed{};
                    if (shape->kind == JsonValue::Kind::String &&
                        shape_from_text(shape->text, parsed)) {
                        slot.shape = parsed;
                    } else if (shape->kind == JsonValue::Kind::Number) {
                        const std::int64_t index = shape->number;
                        const std::int64_t last =
                            static_cast<std::int64_t>(SelectionShape::RoundedRectangle);
                        if (index >= 0 && index <= last) {
                            slot.shape = static_cast<SelectionShape>(index);
                        }
                    }
                }
                read_px_field(item.find("corner_radius_px"), slot.corner_radius_px);
            }
        }
        read_int_field(root.find("selection_slot"), cfg.selection_slot);

        read_q16_field(root.find("factor_q16"), cfg.factor_q16);
        read_size_field(root.find("output_size_px"), cfg.output_size_px);
        read_bool_field(root.find("keep_aspect_ratio"), cfg.keep_aspect_ratio);
        if (const JsonValue* value = root.find("presets");
            value != nullptr && value->kind == JsonValue::Kind::Array) {
            const std::size_t count = value->items.size() < kPresetCount
                                          ? value->items.size()
                                          : static_cast<std::size_t>(kPresetCount);
            for (std::size_t i = 0; i < count; ++i) {
                read_q16_field(&value->items[i], cfg.presets[i]);
            }
        }

        read_bool_field(root.find("output_position_auto"), cfg.output_position_auto);
        read_point_field(root.find("output_position_px"), cfg.output_position_px);

        read_bool_field(root.find("start_in_pass_through"), cfg.start_in_pass_through);
        read_bool_field(root.find("edge_dwell_enabled"), cfg.edge_dwell_enabled);
        read_px_field(root.find("edge_dwell_band_px"), cfg.edge_dwell_band_px);
        read_int_field(root.find("edge_dwell_ms"), cfg.edge_dwell_ms);

        read_bool_field(root.find("strict_compat_mode"), cfg.strict_compat_mode);
        if (const JsonValue* value = root.find("scale_filter"); value != nullptr) {
            ScaleFilter filter{};
            if (value->kind == JsonValue::Kind::String && filter_from_text(value->text, filter)) {
                cfg.scale_filter = filter;
            } else if (value->kind == JsonValue::Kind::Number) {
                const std::int64_t index = value->number;
                if (index >= 0 && index <= static_cast<std::int64_t>(ScaleFilter::Bicubic)) {
                    cfg.scale_filter = static_cast<ScaleFilter>(index);
                }
            }
        }
        read_int_field(root.find("target_fps"), cfg.target_fps);
        read_bool_field(root.find("exclude_self_from_capture"), cfg.exclude_self_from_capture);
        read_bool_field(root.find("show_border"), cfg.show_border);
        if (const JsonValue* value = root.find("language");
            value != nullptr && value->kind == JsonValue::Kind::String) {
            Language parsed{};
            // An unknown code leaves the current language alone rather than
            // silently resetting the user's choice.
            if (language_from_code(value->text.c_str(), parsed)) cfg.language = parsed;
        }

        if (const JsonValue* value = root.find("hotkeys");
            value != nullptr && value->kind == JsonValue::Kind::Object) {
            for (const auto& entry : value->members) {
                HotkeyAction action{};
                if (!hotkey_action_from_name(entry.first.c_str(), action)) continue;
                if (static_cast<std::size_t>(action) >= kHotkeyCount) continue;
                HotkeyChord chord{};
                if (!read_chord_field(&entry.second, chord)) continue;
                cfg.hotkeys[static_cast<std::size_t>(action)] = chord;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::string describe_chord(const HotkeyChord& chord) {
    // An unbound chord has no text; parse_chord() maps "" straight back to it.
    if (!chord_is_bound(chord)) return std::string();

    std::string out;
    if ((chord.modifiers & kModControl) != 0) out += "Ctrl+";
    if ((chord.modifiers & kModAlt) != 0) out += "Alt+";
    if ((chord.modifiers & kModShift) != 0) out += "Shift+";
    if ((chord.modifiers & kModWin) != 0) out += "Win+";
    out += describe_virtual_key(chord.virtual_key);
    return out;
}

bool parse_chord(const std::string& text, HotkeyChord& out) noexcept {
    try {
        std::size_t begin = 0;
        std::size_t end = text.size();
        while (begin < end && is_blank(text[begin])) ++begin;
        while (end > begin && is_blank(text[end - 1])) --end;
        if (begin == end) {
            out = HotkeyChord{};
            return true;
        }

        std::uint32_t modifiers = 0;
        std::uint32_t vk = 0;
        std::size_t start = begin;
        for (std::size_t i = begin; i <= end; ++i) {
            if (i != end && text[i] != '+') continue;
            std::string token = text.substr(start, i - start);
            start = i + 1;
            std::size_t token_begin = 0;
            std::size_t token_end = token.size();
            while (token_begin < token_end && is_blank(token[token_begin])) ++token_begin;
            while (token_end > token_begin && is_blank(token[token_end - 1])) --token_end;
            token = token.substr(token_begin, token_end - token_begin);

            if (token.empty()) {
                // "Ctrl+Alt++" spells the plus key with a bare '+'.
                vk = kVkOemPlus;
                continue;
            }
            std::uint32_t bit = 0;
            if (modifier_bit(token, bit)) {
                modifiers |= bit;
                continue;
            }
            std::uint32_t key = 0;
            if (!vk_from_name(token, key)) return false;
            vk = key;
        }
        out = HotkeyChord{modifiers, vk};
        return true;
    } catch (...) {
        return false;
    }
}

std::string describe_virtual_key(std::uint32_t vk) {
    if (vk == 0) return "None";
    if (vk >= static_cast<std::uint32_t>('A') && vk <= static_cast<std::uint32_t>('Z')) {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= static_cast<std::uint32_t>('0') && vk <= static_cast<std::uint32_t>('9')) {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= kVkF1 && vk <= kVkF24) return "F" + std::to_string(vk - kVkF1 + 1);
    for (std::size_t i = 0; i < kKeyNameCount; ++i) {
        if (kKeyNames[i].vk == vk) return kKeyNames[i].name;
    }
    return hex_token(vk);
}

// ---------------------------------------------------------------------------
// ConfigStore
// ---------------------------------------------------------------------------

struct ConfigStore::Impl {
    void run();

    std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    std::string path;
    std::string write_error;
    AppConfig pending{};
    bool has_path{false};
    bool has_pending{false};
    bool stop{false};
};

void ConfigStore::Impl::run() {
    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
        cv.wait(lock, [this] { return stop || has_pending; });
        if (!has_pending) return;  // stop requested with nothing left to drain

        const std::string target = path;
        const AppConfig snapshot = pending;
        has_pending = false;
        lock.unlock();

        std::string error;
        write_config_file(target, snapshot, error);

        lock.lock();
        write_error = error;
    }
}

ConfigStore::ConfigStore() : impl_(new Impl()) {}

ConfigStore::~ConfigStore() {
    flush();
    delete impl_;
    impl_ = nullptr;
}

std::string ConfigStore::default_path() {
    std::filesystem::path base;
    if (const char* appdata = std::getenv("APPDATA"); appdata != nullptr && *appdata != '\0') {
        base = appdata;
    } else if (const char* profile = std::getenv("USERPROFILE");
               profile != nullptr && *profile != '\0') {
        base = std::filesystem::path(profile) / "AppData" / "Roaming";
    } else {
        base = std::filesystem::path(".");
    }

    const std::filesystem::path directory = base / "Magnifier";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    return (directory / "config.json").string();
}

bool ConfigStore::load_now(const std::string& path, AppConfig& out, const AppConfig& fallback) {
    if (impl_ != nullptr) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->path = path;
        impl_->has_path = true;
    }

    std::error_code ec;
    const bool present = std::filesystem::exists(path, ec);
    if (!present || ec) {
        out = fallback;  // a first run has no file yet, which is not an error
        last_error_.clear();
        return true;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        out = fallback;
        last_error_ = "cannot open config file: " + path;
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (stream.bad() || text.size() > kMaxConfigBytes) {
        out = fallback;
        last_error_ = "cannot read config file: " + path;
        return false;
    }

    // The fallback seeds the untouched fields, so a partial file upgrades the
    // shipped defaults instead of resetting them to zero.
    out = fallback;
    if (!from_json(text, out)) {
        out = fallback;
        last_error_ = "config file is not usable JSON: " + path;
        return false;
    }
    last_error_.clear();
    return true;
}

void ConfigStore::save_async(const AppConfig& cfg) {
    if (impl_ == nullptr) return;

    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (!impl_->has_path) {
        impl_->path = default_path();
        impl_->has_path = true;
    }
    impl_->pending = cfg;
    impl_->has_pending = true;
    if (!impl_->worker.joinable()) {
        impl_->stop = false;
        try {
            impl_->worker = std::thread([impl = impl_] { impl->run(); });
        } catch (...) {
            impl_->has_pending = false;
            last_error_ = "cannot start the config writer thread";
            return;
        }
    }
    lock.unlock();
    impl_->cv.notify_one();
}

void ConfigStore::flush() {
    if (impl_ == nullptr) return;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stop = true;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->write_error.empty()) last_error_ = impl_->write_error;
}

}  // namespace mag
