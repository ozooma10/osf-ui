#include "../../tools/cef_host/FramePacer.h"
#include "../../tools/cef_host/FrameSchedule.h"
#include "../../tools/cef_host/RevealState.h"
#include "check.h"

using osfui::cefhost::FramePacer;
using namespace std::chrono_literals;

int main()
{
    FramePacer pacer;
    const auto start = FramePacer::Clock::time_point{};
    CHECK(pacer.Remaining(start) == 0ns);
    CHECK(pacer.CoalescingRemaining(start) == 0ns);
    pacer.Published(start);
    CHECK(pacer.CoalescingRemaining(start + FramePacer::period) == 4ms);
    CHECK(pacer.CoalescingRemaining(start + FramePacer::period + 4ms) == 0ns);
    FramePacer coalesced;
    coalesced.Published(start);
    coalesced.Published(start + FramePacer::period + 4ms);
    CHECK(coalesced.Remaining(start + FramePacer::period + 4ms) == FramePacer::period - 4ms);
    CHECK(pacer.Remaining(start) == FramePacer::period);
    CHECK(pacer.Remaining(start + 10ms) == FramePacer::period - 10ms);
    CHECK(pacer.Remaining(start + 16ms) == 666667ns);

    // Three browsers offering updates every millisecond still share 60 slots.
    FramePacer busy;
    int frames = 0;
    for (int ms = 0; ms < 1000; ++ms) for (int view = 0; view < 3; ++view) {
        const auto now = start + std::chrono::milliseconds(ms);
        if (busy.Remaining(now) == 0ns) { ++frames; busy.Published(now); }
    }
    CHECK(frames == 60);

    // Small scheduling lateness does not become a new full-period sleep.
    FramePacer delayed;
    delayed.Published(start);
    delayed.Published(start + 19ms);
    CHECK(delayed.Remaining(start + 19ms) == 2 * FramePacer::period - 19ms);
    delayed.Published(start + 35ms);
    CHECK(delayed.Remaining(start + 35ms) == 3 * FramePacer::period - 35ms);

    // A backpressure/visibility stall does not permit a catch-up burst.
    CHECK(delayed.Remaining(start + 1s) == 0ns);
    CHECK(delayed.CoalescingRemaining(start + 1s) == 0ns);
    delayed.Published(start + 1s);
    CHECK(delayed.Remaining(start + 1s) == FramePacer::period);
    CHECK(delayed.Remaining(start + 1010ms) == FramePacer::period - 10ms);

    osfui::cefhost::FrameSchedule output;
    CHECK(!output.Queue(start));
    output.Invalidate();
    CHECK(output.Queue(start) == 0ns);
    output.Invalidate(); // More paints replace pixels without another wake.
    CHECK(!output.Queue(start));
    CHECK(output.Woke());
    output.Blocked();
    CHECK(output.Pending());
    CHECK(!output.Queue(start));
    CHECK(!output.Woke());
    output.Unblock();
    CHECK(output.Queue(start) == 0ns);
    CHECK(output.Woke());
    output.Published(start);
    CHECK(!output.Pending());
    output.Invalidate();
    CHECK(output.Queue(start + 10ms) == FramePacer::period - 10ms);
    CHECK(output.Woke());
    CHECK(output.Queue(start + FramePacer::period, true) == 4ms);
    CHECK(output.Woke());
    CHECK(!output.Queue(start + FramePacer::period + 4ms, true));
    output.Published(start + FramePacer::period + 4ms);
    output.Invalidate();
    CHECK(output.Queue(start + 1s) == 0ns);
    CHECK(output.Woke());
    output.NoViews();
    CHECK(!output.Queue(start + 1s));
    output.Invalidate();
    CHECK(output.Queue(start + 1s) == 0ns);
    output.Stop(); // A posted timer callback cannot resurrect shutdown work.
    CHECK(!output.Woke());
    output.Unblock(); output.Invalidate();
    CHECK(!output.Pending() && !output.Queue(start + 2s));

    osfui::cefhost::RevealState reveal;
    CHECK(!reveal.Ready() && !reveal.Sentinel());
    reveal.Begin(1);
    reveal.Painted(); // Show paint arrived before the renderer sentinel.
    CHECK(!reveal.Ready());
    CHECK(reveal.Sentinel());
    CHECK(!reveal.Sentinel()); // Duplicate sentinels do not restart the gate.
    CHECK(reveal.NeedsRefresh(1) && !reveal.NeedsRefresh(2));
    reveal.Begin(2); // Resize/show invalidates a queued retry for the old reveal.
    CHECK(!reveal.NeedsRefresh(1));
    CHECK(reveal.Sentinel());
    reveal.Painted();
    CHECK(reveal.Ready() && !reveal.NeedsRefresh(2));
    reveal.Reset(); // Hide, navigation, and renderer loss invalidate the gate.
    CHECK(!reveal.Ready() && !reveal.Sentinel() && !reveal.NeedsRefresh(2));
    std::printf("cef_frame_pacer_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
