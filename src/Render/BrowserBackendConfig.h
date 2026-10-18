#pragma once

#include "BrowserBackend.h"
#include <filesystem>
#include <string>

namespace OSFUI
{
    // Optional package-owned selection, independent of saved player settings.
    // Missing configuration defaults to WebView2; unreadable or invalid files are errors.
    std::optional<osfui::browser::Backend> ReadBrowserBackend(
        const std::filesystem::path& dataDir, std::string& error);
}
