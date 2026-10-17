#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>

namespace osfui::cefhost
{
    struct PaintRect
    {
        int x{}, y{}, width{}, height{};
        std::int64_t Right() const { return std::int64_t(x) + width; }
        std::int64_t Bottom() const { return std::int64_t(y) + height; }
        bool operator==(const PaintRect&) const = default;
    };

    // CEF damage is relative to coded texture pixels and the directly preceding
    // capture counter. Missing/dropped frames require a full refresh. Keep this
    // history with the owned pixels; clearing a surface also clears its history.
    class PaintDamage
    {
    public:
        template<class Rects>
        PaintRect Select(PaintRect visible, const Rects& dirty, std::optional<std::uint64_t> counter) const
        {
            if (!counter || !counter_ || *counter <= *counter_ || *counter - *counter_ != 1 ||
                visible != visible_ || dirty.empty()) return visible;
            auto left = visible.Right(), top = visible.Bottom();
            auto right = std::int64_t(visible.x), bottom = std::int64_t(visible.y);
            for (const auto& rect : dirty) {
                if (rect.width <= 0 || rect.height <= 0) return visible;
                const auto x = (std::max)(std::int64_t(visible.x), std::int64_t(rect.x));
                const auto y = (std::max)(std::int64_t(visible.y), std::int64_t(rect.y));
                const auto r = (std::min)(visible.Right(), std::int64_t(rect.x) + rect.width);
                const auto b = (std::min)(visible.Bottom(), std::int64_t(rect.y) + rect.height);
                if (r <= x || b <= y) continue;
                left = (std::min)(left, x); top = (std::min)(top, y);
                right = (std::max)(right, r); bottom = (std::max)(bottom, b);
            }
            // One bounded copy also handles overlapping rectangles without an
            // allocation or one GPU command per rectangle. Empty damage is
            // conservatively treated as a refresh, as in CEF's OSR consumer.
            if (right <= left || bottom <= top) return visible;
            return { int(left), int(top), int(right - left), int(bottom - top) };
        }
        void Copied(PaintRect visible, std::optional<std::uint64_t> counter)
        { visible_ = visible; counter_ = counter; }
    private:
        PaintRect visible_;
        std::optional<std::uint64_t> counter_;
    };
}
