#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace osfui::cefhost
{
    // CEF declares errorText optional. Never publish an empty failure to chrome.
    inline std::string DescribeLoadError(int code, std::string text)
    {
        return text.empty() ? "CEF navigation failed (error " + std::to_string(code) + ")" : std::move(text);
    }

    // CEF calls OnLoadStart at commit, then OnLoadError and/or OnLoadEnd.
    // A navigation request (including a same-URL reload) is not a commit and
    // must not clear a failure while the old document is still ending its load.
    class LoadState
    {
    public:
        void Start(std::string frame) { frame_ = std::move(frame); awaitingEnd_ = true; }
        void Fail(std::string frame) { frame_ = std::move(frame); awaitingEnd_ = false; }
        std::optional<bool> End(std::string_view frame, bool documentAllowed)
        {
            if (!awaitingEnd_ || frame != frame_) return std::nullopt;
            awaitingEnd_ = false;
            return documentAllowed;
        }
    private:
        std::string frame_;
        bool awaitingEnd_{};
    };
}
