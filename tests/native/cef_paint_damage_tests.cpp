#include "../../tools/cef_host/PaintDamage.h"
#include "check.h"
#include <array>
#include <limits>
#include <vector>

using namespace osfui::cefhost;
int main()
{
    PaintDamage history;
    const PaintRect visible{ 2, 3, 8, 8 }, patch{ 4, 5, 2, 2 };
    std::vector<PaintRect> dirty{ patch };
    CHECK(history.Select(visible, dirty, 10) == visible);
    history.Copied(visible, 10);
    CHECK(history.Select(visible, dirty, 11) == patch);
    CHECK(history.Select(visible, dirty, 12) == visible); // Missed a capture.
    CHECK(history.Select(visible, dirty, 10) == visible);
    CHECK(history.Select(visible, dirty, 9) == visible);
    CHECK(history.Select(visible, dirty, std::nullopt) == visible);
    CHECK(history.Select(PaintRect{ 1, 3, 8, 8 }, dirty, 11) == (PaintRect{ 1, 3, 8, 8 }));
    CHECK(history.Select(PaintRect{ 2, 3, 9, 8 }, dirty, 11) == (PaintRect{ 2, 3, 9, 8 }));
    dirty = { { 0, 1, 5, 5 }, { 8, 8, 8, 8 } };
    CHECK(history.Select(visible, dirty, 11) == visible); // Clip and union in coded coordinates.
    dirty = { { -10, -10, 14, 15 } };
    CHECK(history.Select(visible, dirty, 11) == (PaintRect{ 2, 3, 2, 2 }));
    dirty = { { 100, 100, 4, 4 } };
    CHECK(history.Select(visible, dirty, 11) == visible);
    dirty = { { 4, 5, -1, 2 } };
    CHECK(history.Select(visible, dirty, 11) == visible);
    dirty.clear();
    CHECK(history.Select(visible, dirty, 11) == visible);
    dirty = { patch };
    history.Copied(visible, std::nullopt);
    CHECK(history.Select(visible, dirty, 11) == visible);
    history.Copied(visible, (std::numeric_limits<std::uint64_t>::max)());
    CHECK(history.Select(visible, dirty, 0) == visible);
    history = {};
    CHECK(history.Select(visible, dirty, 11) == visible);

    // Apply selected regions to persistent pixels. A dropped capture changes a
    // different area than the next damage rect: only a full refresh repairs it.
    std::array<int,64> source{}, owned{};
    auto copy = [&](std::uint64_t counter, PaintRect damage) {
        const auto region = history.Select(visible, std::array{damage}, counter);
        for (int y=region.y; y<region.Bottom(); ++y)
            for (int x=region.x; x<region.Right(); ++x)
                owned[(y-visible.y)*8+x-visible.x] = source[(y-visible.y)*8+x-visible.x];
        history.Copied(visible, counter);
        CHECK(owned == source);
    };
    source.fill(1); copy(1, patch);
    source[2*8+2] = 2; copy(2, {4,5,1,1});
    source[0] = 3; // Capture 3 never reaches the callback.
    source[7*8+7] = 4; copy(4, {9,10,1,1});
    source[2*8+2] = 0; copy(5, {4,5,1,1}); // Erasing remains a copied change.
    std::printf("cef_paint_damage_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
