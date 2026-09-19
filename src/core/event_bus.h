// core/event_bus.h — thread-safe delivery of value events plus the
// "latest snapshot wins" mailbox used to feed the render thread.
//
// Constraint (design doc §4.4/§4.5): queues are bounded; input and config
// commands must never be silently lost, while render snapshots and captured
// frames may be dropped because only the newest one matters.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "core/events.h"
#include "core/types.h"

namespace mag {

class IEventBus {
public:
    virtual ~IEventBus() = default;

    // Thread-safe. Returns false when a bounded queue is full and the event
    // was rejected. Implementations must not throw.
    virtual bool publish(const AppEvent& event) noexcept = 0;

    // Consumes up to max_events events on the owning thread.
    // Throws std::invalid_argument when max_events == 0.
    virtual std::size_t drain(std::size_t max_events) = 0;
};

// Bounded multi-producer / single-consumer queue.
//
// A simple mutex is the right call here: the queue carries human-scale traffic
// (a few dozen events per second), so contention is irrelevant, and lock-free
// code would be a correctness liability for no measurable gain.
class BoundedEventBus final : public IEventBus {
public:
    explicit BoundedEventBus(std::size_t capacity = 1024);

    bool publish(const AppEvent& event) noexcept override;
    std::size_t drain(std::size_t max_events) override;

    // Invoked on the draining thread for each event, in order.
    using Handler = void (*)(void* ctx, const AppEvent& event);
    void set_handler(Handler handler, void* ctx) noexcept;

    std::size_t size() const noexcept;
    std::uint64_t dropped_count() const noexcept;

private:
    mutable std::mutex mutex_;
    std::deque<AppEvent> queue_;
    std::size_t capacity_;
    std::uint64_t dropped_{0};
    Handler handler_{nullptr};
    void* handler_ctx_{nullptr};
};

// Single-slot mailbox holding the newest immutable render state.
//
// The render thread always reads the latest snapshot and never queues up
// stale ones: a magnifier that rendered a 2-frame-old magnification level
// would feel laggy, which is exactly what the design forbids.
class SnapshotMailbox {
public:
    void submit(const RenderSnapshot& snapshot) noexcept;

    struct Entry {
        RenderSnapshot snapshot;
        std::uint64_t sequence{0};
    };

    // Returns the newest snapshot if one has been submitted since last_seq.
    // On success `last_seq` is updated to the consumed sequence.
    bool take_latest(Entry& out, std::uint64_t& last_seq) const noexcept;

    // Reads the current snapshot regardless of sequence (used by the capture
    // thread, which needs the live selection to know what to crop).
    bool peek(RenderSnapshot& out) const noexcept;

    std::uint64_t sequence() const noexcept;

private:
    mutable std::mutex mutex_;
    RenderSnapshot snapshot_{};
    std::uint64_t sequence_{0};
    bool has_value_{false};
};

}  // namespace mag
