// core/event_bus.cpp — bounded event queue plus the latest-snapshot mailbox.
#include "core/event_bus.h"

#include <cstring>
#include <stdexcept>
#include <utility>

namespace mag {
namespace {

struct HotkeyName {
    HotkeyAction action;
    const char* name;
};

// Single source of truth for the persisted names, so the two directions of the
// mapping cannot drift apart.
constexpr HotkeyName kHotkeyNames[] = {
    {HotkeyAction::ToggleMagnifier, "ToggleMagnifier"},
    {HotkeyAction::TogglePassThrough, "TogglePassThrough"},
    {HotkeyAction::CycleShape, "CycleShape"},
    {HotkeyAction::ZoomIn, "ZoomIn"},
    {HotkeyAction::ZoomOut, "ZoomOut"},
    {HotkeyAction::Preset1, "Preset1"},
    {HotkeyAction::Preset2, "Preset2"},
    {HotkeyAction::Preset3, "Preset3"},
    {HotkeyAction::Preset4, "Preset4"},
    {HotkeyAction::GrowWidth, "GrowWidth"},
    {HotkeyAction::ShrinkWidth, "ShrinkWidth"},
    {HotkeyAction::GrowHeight, "GrowHeight"},
    {HotkeyAction::ShrinkHeight, "ShrinkHeight"},
    {HotkeyAction::ResetSelection, "ResetSelection"},
    {HotkeyAction::CenterOutput, "CenterOutput"},
    {HotkeyAction::Quit, "Quit"},
};

}  // namespace

// ---------------------------------------------------------------------------
// events.h free functions
// ---------------------------------------------------------------------------

const char* hotkey_action_name(HotkeyAction a) noexcept {
    for (const HotkeyName& entry : kHotkeyNames) {
        if (entry.action == a) return entry.name;
    }
    return "?";
}

bool hotkey_action_from_name(const char* name, HotkeyAction& out) noexcept {
    if (name == nullptr) return false;
    for (const HotkeyName& entry : kHotkeyNames) {
        if (std::strcmp(entry.name, name) == 0) {
            out = entry.action;
            return true;
        }
    }
    return false;
}

AppEvent make_event(Payload payload) noexcept {
    AppEvent e;
    e.payload = std::move(payload);
    return e;
}

AppEvent make_event(Payload payload, std::uint64_t timestamp_qpc,
                    std::uint32_t source_thread) noexcept {
    AppEvent e;
    e.payload = std::move(payload);
    e.timestamp_qpc = timestamp_qpc;
    e.source_thread = source_thread;
    return e;
}

// ---------------------------------------------------------------------------
// BoundedEventBus
// ---------------------------------------------------------------------------

BoundedEventBus::BoundedEventBus(std::size_t capacity) : capacity_(capacity) {}

bool BoundedEventBus::publish(const AppEvent& event) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= capacity_) {
        ++dropped_;
        return false;
    }
    try {
        queue_.push_back(event);
    } catch (...) {
        // publish() is contractually noexcept: an allocation failure is a lost
        // event, never a propagated exception.
        ++dropped_;
        return false;
    }
    return true;
}

std::size_t BoundedEventBus::drain(std::size_t max_events) {
    if (max_events == 0) {
        throw std::invalid_argument("BoundedEventBus::drain: max_events must be greater than zero");
    }

    Handler handler = nullptr;
    void* ctx = nullptr;
    std::vector<AppEvent> batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t count = queue_.size() < max_events ? queue_.size() : max_events;
        handler = handler_;
        ctx = handler_ctx_;
        if (handler == nullptr) {
            for (std::size_t i = 0; i < count; ++i) queue_.pop_front();
            return count;
        }
        // Handlers run outside the lock, so one that publishes cannot deadlock.
        batch.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            batch.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
    }
    for (const AppEvent& event : batch) handler(ctx, event);
    return batch.size();
}

void BoundedEventBus::set_handler(Handler handler, void* ctx) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    handler_ = handler;
    handler_ctx_ = ctx;
}

std::size_t BoundedEventBus::size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

std::uint64_t BoundedEventBus::dropped_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

// ---------------------------------------------------------------------------
// SnapshotMailbox
// ---------------------------------------------------------------------------

void SnapshotMailbox::submit(const RenderSnapshot& snapshot) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = snapshot;
    ++sequence_;
    has_value_ = true;
}

bool SnapshotMailbox::take_latest(Entry& out, std::uint64_t& last_seq) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_value_ || sequence_ == last_seq) return false;
    out.snapshot = snapshot_;
    out.sequence = sequence_;
    last_seq = sequence_;
    return true;
}

bool SnapshotMailbox::peek(RenderSnapshot& out) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_value_) return false;
    out = snapshot_;
    return true;
}

std::uint64_t SnapshotMailbox::sequence() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return sequence_;
}

}  // namespace mag
