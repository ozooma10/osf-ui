#include "../../tools/cef_host/FrameSchedule.h"
#include "../../tools/cef_host/RevealState.h"
#include "check.h"

#include <algorithm>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace
{
    using osfui::cefhost::FramePacer;
    struct Surface { bool texture{}; };
    struct Rect { int x{}, y{}, width{ 1920 }, height{ 1080 }; };
    struct View
    {
        bool hidden{}, closing{}, loaded{}, popupShown{};
        osfui::cefhost::RevealState reveal;
        Surface surface, popup;
        Rect popupRect;
        std::uint64_t paintSerial{}, publishedPaintSerial{};
        FramePacer::Clock::time_point lastPaint{};
        int order{};
        double Scale() const { return 1; }
    };
    struct Layer { Surface* surface; float x, y, width, height; };
    struct CompositionHarness
    {
        osfui::cefhost::FrameSchedule output;
        std::map<std::string, std::unique_ptr<View>> views;
        unsigned width{ 1920 }, height{ 1080 }, viewportWidth{ 1920 }, viewportHeight{ 1080 };
        std::uint64_t epoch{ 1 };
        struct Graphics
        {
            unsigned calls{};
            bool available{ true }, complete{};
            std::vector<Layer> layers;
            bool Publish(std::span<const Layer> input, unsigned, unsigned, std::uint64_t, bool allReady)
            {
                ++calls; complete = allReady; layers.assign(input.begin(), input.end());
                return available;
            }
        } graphics;
        Rect ViewBounds(const View&) const { return {}; }
        bool ScheduleFrame(bool) { return false; }

        // Run the production composition policy with only GPU publication stubbed.
#include "../../tools/cef_host/HostComposition.inl"
    };
    void PaintReady(View& view)
    {
        view.loaded = true;
        view.reveal.Begin(1);
        view.reveal.Sentinel();
        view.reveal.Painted();
        view.surface.texture = true;
        ++view.paintSerial;
    }
}

int main()
{
    CompositionHarness host;
    auto& view = *host.views.emplace("menu", std::make_unique<View>()).first->second;
    host.output.Invalidate();
    host.Compose(); // Requested, but not loaded: never publish an empty frame.
    CHECK(host.graphics.calls == 0 && host.output.Pending());
    view.loaded = true; view.surface.texture = true;
    view.reveal.Begin(1); view.reveal.Painted();
    host.Compose(); // A paint before the reveal sentinel is not presentable.
    CHECK(host.graphics.calls == 0 && host.output.Pending());
    view.reveal.Sentinel(); host.Compose();
    CHECK(host.graphics.calls == 0 && host.output.Pending());
    view.reveal.Painted(); view.surface.texture = false; host.Compose();
    CHECK(host.graphics.calls == 0 && host.output.Pending());
    PaintReady(view); host.Compose();
    CHECK(host.graphics.calls == 1 && host.graphics.complete && !host.output.Pending());
    CHECK(host.graphics.layers.size() == 1 && host.graphics.layers[0].surface == &view.surface);
    CHECK(view.publishedPaintSerial == view.paintSerial);

    auto& sibling = *host.views.emplace("sibling", std::make_unique<View>()).first->second;
    host.output.Invalidate(); host.Compose();
    CHECK(host.graphics.calls == 2 && !host.graphics.complete); // Healthy view keeps updating.
    CHECK(host.graphics.layers.size() == 1 && host.graphics.layers[0].surface == &view.surface);
    view.hidden = true; host.output.Invalidate(); host.Compose();
    CHECK(host.graphics.calls == 2 && host.output.Pending()); // Hidden old menu cannot fill a new reveal.
    sibling.loaded = true; sibling.reveal.Begin(2); host.Compose();
    CHECK(host.graphics.calls == 2 && host.output.Pending());
    PaintReady(sibling); host.Compose();
    CHECK(host.graphics.calls == 3 && host.graphics.complete);
    CHECK(host.graphics.layers.size() == 1 && host.graphics.layers[0].surface == &sibling.surface);

    sibling.surface.texture = false; sibling.reveal.Reset();
    host.output.Invalidate(); host.Compose(); // Resize/navigation invalidates the old surface.
    CHECK(host.graphics.calls == 3 && host.output.Pending());
    PaintReady(sibling); host.graphics.available = false; host.Compose();
    CHECK(host.graphics.calls == 4 && host.output.Pending());
    CHECK(sibling.publishedPaintSerial != sibling.paintSerial); // Full ring retains pending work.
    host.output.Unblock(); host.graphics.available = true; host.Compose();
    CHECK(host.graphics.calls == 5 && !host.output.Pending());
    CHECK(sibling.publishedPaintSerial == sibling.paintSerial);
    sibling.closing = true; host.output.Invalidate(); host.Compose();
    CHECK(host.graphics.calls == 5 && !host.output.Pending()); // All hidden/closing: no work remains.

    std::printf("cef_composition_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
