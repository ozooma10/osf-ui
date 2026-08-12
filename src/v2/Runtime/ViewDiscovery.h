#pragma once

#include "ViewManifest.h"

namespace Runtime
{
    struct ViewDiscoveryIssue
    {
        std::filesystem::path path;
        std::string message;
    };

    struct ViewDiscoveryResult
    {
        std::vector<ViewManifest> views;
        std::vector<ViewDiscoveryIssue> issues;
    };

    ViewDiscoveryResult DiscoverViews(const std::filesystem::path& a_viewsDirectory);
}