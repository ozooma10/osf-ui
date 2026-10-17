#pragma once
#include <chrono>
#include <cstdint>

namespace osfui::cefhost
{
    // One output cadence for the entire view stack, not one cadence per browser.
    // Advance the deadline, rather than sleeping a full period after each copy.
    class FramePacer
    {
    public:
        using Clock = std::chrono::steady_clock;
        static constexpr auto period = std::chrono::nanoseconds(16'666'667);
        static constexpr auto coalescingWindow = std::chrono::milliseconds(4);

        Clock::duration Remaining(Clock::time_point now) const
        {
            return now >= next_ ? Clock::duration::zero() : next_ - now;
        }
        // A bounded grace period for a recently animating layer whose next
        // paint has not joined this batch. It does not move the 60 Hz deadline.
        Clock::duration CoalescingRemaining(Clock::time_point now) const
        {
            return !started_ || now >= next_ + coalescingWindow
                ? Clock::duration::zero() : next_ + coalescingWindow - now;
        }
        void Published(Clock::time_point now)
        {
            // A stall never earns a burst of catch-up publications.
            if (!started_ || now >= next_ + period) next_ = now + period;
            else next_ += period;
            started_ = true;
        }
    private:
        Clock::time_point next_{};
        bool started_{};
    };
}
