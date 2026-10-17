#pragma once

#include "BrowserBackend.h"
#include <filesystem>
#include <string>

namespace OSFUI
{
    struct BrowserHostLayout
    {
        std::filesystem::path executable;
        std::string profile;
        std::string log;
    };

    BrowserHostLayout HostLayout(osfui::browser::Backend backend, const std::filesystem::path& dataDir);
    bool ValidateHostPayload(osfui::browser::Backend backend, const std::filesystem::path& executable,
        std::string& error);
    std::optional<std::filesystem::path> PrepareHostExecutable(osfui::browser::Backend backend,
        const std::filesystem::path& source, const std::filesystem::path& localRoot);
}
