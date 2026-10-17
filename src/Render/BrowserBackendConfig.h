#pragma once

#include "BrowserBackend.h"
#include <filesystem>
#include <string>

namespace OSFUI
{
    // Package-owned selection, independent of saved player settings. Missing or invalid
    // configuration is an installation error; never silently start a different backend.
    std::optional<osfui::browser::Backend> ReadBrowserBackend(
        const std::filesystem::path& dataDir, std::string& error);
}
