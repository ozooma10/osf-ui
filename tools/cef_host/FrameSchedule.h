#pragma once
#include <optional>
#include "FramePacer.h"

namespace osfui::cefhost
{
    // CEF UI-thread publication state. Exactly one wake may be queued; a full
    // ring retains dirty work until an acknowledgment or resize unblocks it.
    class FrameSchedule
    {
    public:
        using Clock = FramePacer::Clock;
        void Invalidate() { if (wake_ != Wake::Stopped) dirty_ = true; }
        bool Pending() const { return dirty_; }
        Clock::duration Remaining(Clock::time_point now) const { return pacer_.Remaining(now); }
        std::optional<Clock::duration> Queue(Clock::time_point now, bool coalesce = false)
        {
            if (!dirty_ || wake_ != Wake::Idle) return std::nullopt;
            const auto delay = coalesce ? pacer_.CoalescingRemaining(now) : pacer_.Remaining(now);
            if (coalesce && delay == Clock::duration::zero()) return std::nullopt;
            wake_ = Wake::Queued;
            return delay;
        }
        bool Woke()
        {
            if (wake_ != Wake::Queued) return false;
            wake_ = Wake::Idle;
            return dirty_;
        }
        void Published(Clock::time_point now) { dirty_ = false; pacer_.Published(now); }
        void NoViews() { dirty_ = false; }
        void Blocked() { wake_ = Wake::Blocked; }
        void Unblock() { if (wake_ == Wake::Blocked) wake_ = Wake::Idle; }
        void Stop() { dirty_ = false; wake_ = Wake::Stopped; }
    private:
        enum class Wake { Idle, Queued, Blocked, Stopped };
        Wake wake_{ Wake::Idle };
        bool dirty_{};
        FramePacer pacer_;
    };
}
