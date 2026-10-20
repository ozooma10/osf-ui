#pragma once
#include "BrowserContentPolicy.h"
#include <algorithm>
#include <optional>
#include <string>

namespace osfui::browser
{
    inline constexpr std::string_view kBrowserView = "osfui.browser/tv";
    inline constexpr std::string_view kBrowserContent = "osfui.browser/content";

    // URL-like input is validated, never converted into a search after rejection.
    // Unicode query text is UTF-8 percent encoded. IDN authorities must use IDNA.
    inline std::optional<std::string> BrowserDestination(std::string input, std::string_view engine = "duckduckgo")
    {
        const auto first = input.find_first_not_of(" \t\r\n");
        if (first == input.npos) return {};
        input = input.substr(first, input.find_last_not_of(" \t\r\n") - first + 1);
        if (input.size() > 8192) return {};
        std::string lower = input;
        for (auto& ch : lower) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        const auto colon = input.find(':');
        const auto slash = input.find_first_of("/?#");
        const bool authorityPort = colon != input.npos && (slash == input.npos || colon < slash) &&
            (input.starts_with('[') || input.substr(0, colon).find('.') != input.npos);
        const bool scheme = colon != input.npos && (slash == input.npos || colon < slash) && !authorityPort;
        const bool urlLike = scheme || input.starts_with("//") ||
            (input.find(' ') == input.npos && (input.find('.') != input.npos ||
                input.starts_with('[') || lower == "localhost"));
        if (urlLike) {
            if (scheme && !lower.starts_with("http://") && !lower.starts_with("https://")) return {};
            if (!scheme) input = "https:" + (input.starts_with("//") ? input : "//" + input);
            // UTF-8 path bytes are percent encoded so the shared wide URI gate
            // can validate an ASCII authority without lossy conversions.
            std::string encoded;
            const auto authorityEnd = input.find_first_of("/?#", input.find("://") + 3);
            constexpr char hex[] = "0123456789ABCDEF";
            for (std::size_t i = 0; i < input.size(); ++i) {
                const auto ch = static_cast<unsigned char>(input[i]);
                if (ch >= 128) {
                    if (i < authorityEnd) return {};
                    encoded += '%'; encoded += hex[ch >> 4]; encoded += hex[ch & 15];
                } else encoded += static_cast<char>(ch);
            }
            if (!IsAllowedRemoteNavigationUri(std::wstring(encoded.begin(), encoded.end()))) return {};
            return encoded;
        }
        std::string url;
        if (engine == "duckduckgo") url = "https://duckduckgo.com/?q=";
        else if (engine == "google") url = "https://www.google.com/search?q=";
        else if (engine == "bing") url = "https://www.bing.com/search?q=";
        else return {};
        constexpr char hex[] = "0123456789ABCDEF";
        for (const unsigned char ch : input) {
            if (ch < 32 || ch == 127) return {};
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') url += ch;
            else { url += '%'; url += hex[ch >> 4]; url += hex[ch & 15]; }
        }
        return url;
    }

    enum class BrowserPanel { None, Chrome, Content };
    struct PanelRect
    {
        int x{}, y{}, width{}, height{};
        bool Contains(int px, int py) const { return px >= x && py >= y && px < x + width && py < y + height; }
    };
    inline PanelRect BrowserPanelRect(int width, int height, unsigned band, bool fullscreen, bool landing, BrowserPanel panel)
    {
        const int chrome = landing ? height : fullscreen ? std::clamp<int>(band, 1, (std::max)(1, height - 1)) : 0;
        return panel == BrowserPanel::Chrome ? PanelRect{0, 0, width, chrome} :
            PanelRect{0, chrome, width, height - chrome};
    }
    class BrowserPointerRouter
    {
    public:
        void Reset() { buttons_ = 0; captured_ = BrowserPanel::None; }
        BrowserPanel Route(int x, int y, bool button, unsigned which, bool down,
            PanelRect chrome, PanelRect content)
        {
            auto target = buttons_ ? captured_ : chrome.Contains(x, y) ? BrowserPanel::Chrome :
                content.Contains(x, y) ? BrowserPanel::Content : BrowserPanel::None;
            if (button && which < 3) {
                if (down && target != BrowserPanel::None) { if (!buttons_) captured_ = target; buttons_ |= 1u << which; }
                else if (!down) buttons_ &= ~(1u << which);
            }
            return target; // Release stays with the press even outside both panels.
        }
    private:
        unsigned buttons_{};
        BrowserPanel captured_{BrowserPanel::None};
    };
}
