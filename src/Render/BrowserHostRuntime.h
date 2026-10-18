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
    std::optional<std::filesystem::path> PrepareBrowserBroker(const std::filesystem::path& dataDir,
        const std::filesystem::path& localRoot);
    // Call only after the helper launched from hostExe has connected; older copies are no longer needed.
    void PruneBrowserRuntimeCopies(const std::filesystem::path& localRoot,
        const std::filesystem::path& hostExe, const std::filesystem::path& brokerExe);
}
