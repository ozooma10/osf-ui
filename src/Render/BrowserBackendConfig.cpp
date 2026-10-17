#include "Render/BrowserBackendConfig.h"
#include "Core/Json.h"

namespace OSFUI
{
    std::optional<osfui::browser::Backend> ReadBrowserBackend(
        const std::filesystem::path& dataDir, std::string& error)
    {
        error.clear();
        const auto config = Json::ParseFile(dataDir / "backend.json", &error);
        if (!config) {
            error = "Cannot read OSF UI backend.json. Reinstall one complete OSF UI edition. " + error;
            return std::nullopt;
        }
        if (!config->is_object() || Json::Get(*config, "schemaVersion", 0) != 1) {
            error = "Unsupported OSF UI backend.json schema. Reinstall one complete OSF UI edition.";
            return std::nullopt;
        }
        const auto backend = osfui::browser::ParseBackend(Json::Get(*config, "backend", ""));
        if (!backend) error = "Invalid OSF UI backend.json renderer. Expected webview2 or cef.";
        return backend;
    }
}
