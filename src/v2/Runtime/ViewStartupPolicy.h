#pragma once

#include "ViewManifest.h"

#include <optional>

namespace Runtime
{
    struct DiscoveredViewStartupContext
    {
        bool developerMode {false};
        std::optional<bool> playerOverride;
    };

    bool ShouldAutoStartDiscoveredView(const ViewManifest& a_view, const DiscoveredViewStartupContext& a_context);
}
