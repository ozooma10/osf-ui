#pragma once

#include "ViewManifest.h"

namespace Runtime
{
    class ViewCatalog
    {
    public:
        void Replace(std::vector<ViewManifest> a_views);

        const ViewManifest* Find(std::string_view a_id) const;

        std::span<const ViewManifest> All() const;

    private:
        std::vector<ViewManifest> _views;
    };
}