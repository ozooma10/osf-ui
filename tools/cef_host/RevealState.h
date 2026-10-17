#pragma once
#include <cstdint>

namespace osfui::cefhost
{
    // A document is presentable only after the current reveal's renderer
    // sentinel and a subsequent completed view paint, in that order.
    class RevealState
    {
    public:
        void Reset() { phase_ = Phase::Sentinel; token_ = 0; }
        void Begin(std::uint64_t token) { phase_ = Phase::Sentinel; token_ = token; }
        std::uint64_t Token() const { return token_; }
        bool Sentinel()
        {
            if (!token_ || phase_ != Phase::Sentinel) return false;
            phase_ = Phase::Paint;
            return true;
        }
        void Painted() { if (phase_ == Phase::Paint) phase_ = Phase::Ready; }
        bool Ready() const { return phase_ == Phase::Ready; }
        bool NeedsRefresh(std::uint64_t token) const { return token == token_ && phase_ == Phase::Paint; }
    private:
        enum class Phase { Sentinel, Paint, Ready };
        Phase phase_{ Phase::Sentinel };
        std::uint64_t token_{};
    };
}
