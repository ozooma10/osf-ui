#pragma once

#include <cstdint>

namespace osfui::webview2
{
    // NavigationStarting does not replace the displayed document. Keep its
    // evidence until ContentLoading commits a replacement, including on reload.
    struct DocumentNavigation
    {
        std::uint64_t id{};
        bool loading{}, usable{}, domReady{};
        bool committed{}, errorPage{};

        [[nodiscard]] bool Current(std::uint64_t value) const { return value && value == id; }
        void Start(std::uint64_t value)
        {
            if (Current(value)) return; // Redirects retain the navigation ID.
            id = value;
            loading = true;
            committed = errorPage = false;
        }
        void Commit(std::uint64_t value, bool isErrorPage)
        {
            if (!Current(value) || !loading) return;
            committed = true;
            errorPage = isErrorPage;
            usable = domReady = false;
        }
        bool DomReady(std::uint64_t value)
        {
            if (!Current(value) || errorPage || (!loading && !usable)) return false;
            domReady = true;
            return true;
        }
        void Complete(bool succeeded)
        {
            loading = false;
            usable = succeeded;
        }
        bool Cancel(std::uint64_t value, bool allowedDocument)
        {
            if (!Current(value) || !loading) return false;
            loading = false;
            // A stopped partial document remains browsable. Before commit,
            // retain the previous document instead of claiming the new one loaded.
            usable = allowedDocument && (committed ? !errorPage : usable);
            domReady = usable && (committed || domReady);
            return true;
        }
    };
}
