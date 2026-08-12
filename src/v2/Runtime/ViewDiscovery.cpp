#include "ViewDiscovery.h"

namespace Runtime
{
    ViewDiscoveryResult DiscoverViews(const std::filesystem::path& a_viewsDirectory)
    {
        ViewDiscoveryResult result;

        std::error_code error;

        if (!std::filesystem::is_directory(a_viewsDirectory, error)) {
            result.issues.push_back({
                .path = a_viewsDirectory,
                .message = error ? error.message() : "views directory does not exist"
            });

            return result;
        }

        const auto options = std::filesystem::directory_options::skip_permission_denied;

        for(const auto& modEntry : std::filesystem::directory_iterator(a_viewsDirectory, options)) {

            if (!modEntry.is_directory()) {
                continue;
            }
            if (modEntry.path().filename() == "shared") {
                continue;
            }

            const auto modPath = modEntry.path();

            for(const auto& viewEntry : std::filesystem::directory_iterator(modPath, options)) {
                if (!viewEntry.is_directory()) {
                    continue;
                }

                const auto viewPath = viewEntry.path();
                const auto manifestPath = viewPath / "manifest.json";
                std::error_code manifestError;

                if (!std::filesystem::is_regular_file(manifestPath, manifestError)) {
                    continue;
                }

                auto manifestResult = LoadViewManifest(manifestPath);

                if (!manifestResult) {
                    result.issues.push_back({
                        .path = manifestPath,
                        .message = manifestResult.error()
                    });
                    continue;
                }

                result.views.push_back(std::move(*manifestResult));
            }
        }

        std::ranges::sort(result.views, {}, &ViewManifest::id);

        return result;
    }
}