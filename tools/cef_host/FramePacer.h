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

        Clock::duration Remaining(Clock::time_point now) const
        {
            return now >= next_ ? Clock::duration::zero() : next_ - now;
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
