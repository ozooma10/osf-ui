#include "ViewCatalog.h"

namespace Runtime
{
    void ViewCatalog::Replace(std::vector<ViewManifest> a_views)
    {
        std::ranges::sort(a_views, {}, &ViewManifest::id);
        _views = std::move(a_views);
    }

    const ViewManifest* ViewCatalog::Find(std::string_view a_id) const
    {
        const auto match = std::ranges::find(_views, a_id, &ViewManifest::id);

        return match != _views.end() ? std::addressof(*match) : nullptr;
    }

    std::span<const ViewManifest> ViewCatalog::All() const
    {
        return _views;
    }
}