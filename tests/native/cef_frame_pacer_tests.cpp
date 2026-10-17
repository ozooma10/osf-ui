#include "../../tools/cef_host/FramePacer.h"
#include "check.h"

using osfui::cefhost::FramePacer;
using namespace std::chrono_literals;

int main()
{
    FramePacer pacer;
    const auto start = FramePacer::Clock::time_point{};
    CHECK(pacer.Remaining(start) == 0ns);
    pacer.Published(start);
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
    delayed.Published(start + 1s);
    CHECK(delayed.Remaining(start + 1s) == FramePacer::period);
    CHECK(delayed.Remaining(start + 1010ms) == FramePacer::period - 10ms);
    std::printf("cef_frame_pacer_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
