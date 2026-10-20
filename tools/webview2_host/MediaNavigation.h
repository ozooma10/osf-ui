#pragma once

#include "Core/Json.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace osfui::webview2
{
    // A direct media document can abort the original document request while its
    // native player takes over. Do not generalize this to other failed loads.
    struct MediaNavigation
    {
        static constexpr int kConnectionAborted = 9;
        static constexpr std::uint64_t kTimeoutMs = 2000;
        std::uint64_t navigationId{}, domNavigationId{}, deadline{};
        bool pending{}, queryStarted{};
        std::string source;

        void Start(std::uint64_t id) { *this = {}; navigationId = id; }
        void DomReady(std::uint64_t id) { if (id && id == navigationId) domNavigationId = id; }
        void End() { pending = queryStarted = false; deadline = 0; source.clear(); }

        bool Begin(bool remote, bool succeeded, int error, int httpStatus,
            std::uint64_t id, std::string url, std::uint64_t now)
        {
            if (!remote || succeeded || error != kConnectionAborted || httpStatus != 200 ||
                !id || id != navigationId || url.empty()) return false;
            pending = true;
            queryStarted = false;
            deadline = now + kTimeoutMs;
            source = std::move(url);
            return true;
        }
        [[nodiscard]] bool Expired(std::uint64_t now) const { return pending && now >= deadline; }
        [[nodiscard]] bool Matches(std::uint64_t id, std::uint64_t now) const
        {
            return pending && id && navigationId == id && domNavigationId == id && now < deadline;
        }
        [[nodiscard]] bool Ready(std::uint64_t now) const { return !queryStarted && Matches(navigationId, now); }
    };

    // Inspect only browser-owned main-frame metadata, never page script output or
    // a media subresource/child frame. CDP Frame.url excludes its fragment.
    inline std::string VerifiedMediaDocumentMime(const nlohmann::json& result, std::string_view source)
    {
        namespace Json = OSFUI::Json;
        const auto* tree = Json::GetObject(result, "frameTree");
        const auto* frame = tree ? Json::GetObject(*tree, "frame") : nullptr;
        if (!frame || frame->contains("parentId") || Json::Get(*frame, "id", "").empty() ||
            Json::Get(*frame, "loaderId", "").empty()) return {};
        if (const auto error = frame->find("unreachableUrl"); error != frame->end() &&
            (!error->is_string() || !error->get_ref<const std::string&>().empty())) return {};
        const auto url = Json::Get(*frame, "url", "");
        if (const auto fragment = frame->find("urlFragment"); fragment != frame->end() && !fragment->is_string()) return {};
        if ((!source.starts_with("https://") && !source.starts_with("http://")) ||
            url + Json::Get(*frame, "urlFragment", "") != source) return {};
        const auto mime = Json::Get(*frame, "mimeType", "");
        if ((mime.starts_with("audio/") || mime.starts_with("video/")) && mime.size() > 6 &&
            mime.find_first_of(" \t\r\n;") == std::string::npos) return mime;
        return {};
    }
}
