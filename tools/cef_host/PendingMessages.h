#pragma once
#include <cstddef>
#include <deque>
#include <string>
#include <utility>

namespace osfui::cefhost
{
    // Match WebView2's bounded, drop-oldest pre-load delivery policy.
    class PendingMessages
    {
    public:
        static constexpr std::size_t maxCount = 64, maxBytes = 8u * 1024u * 1024u;
        bool Push(std::string payload)
        {
            if (payload.size() > maxBytes) return false;
            bool retainedAll = true;
            while (!messages_.empty() && (messages_.size() >= maxCount || bytes_ + payload.size() > maxBytes)) {
                bytes_ -= messages_.front().size(); messages_.pop_front(); retainedAll = false;
            }
            bytes_ += payload.size(); messages_.push_back(std::move(payload));
            return retainedAll;
        }
        std::deque<std::string> Take() { bytes_ = 0; return std::exchange(messages_, {}); }
    private:
        std::deque<std::string> messages_;
        std::size_t bytes_{};
    };
}
