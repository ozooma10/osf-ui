#include "ViewDiscovery.h"

namespace Runtime
{
    namespace
    {
        inline constexpr auto kDirectoryOptions = std::filesystem::directory_options::none;

        void AddFilesystemIssue(ViewDiscoveryResult& a_result, const std::filesystem::path& a_path, std::string a_operation, const std::error_code& a_error)
        {
            a_result.issues.push_back({
                .path = a_path,
                .message = std::move(a_operation) + ": " + a_error.message()
            });
        }

        template <class Callback>
        void ForEachDirectory(ViewDiscoveryResult& a_result, const std::filesystem::path& a_parent, std::string_view a_label, Callback&& a_callback)
        {
            std::error_code error;
            std::filesystem::directory_iterator iterator{ a_parent, kDirectoryOptions, error };
            const std::filesystem::directory_iterator end;

            if (error) {
                AddFilesystemIssue(a_result, a_parent, "could not enumerate " + std::string{ a_label }, error);
                return;
            }

            while (iterator != end) {
                const auto entry = *iterator;
                const auto path = entry.path();
                std::error_code typeError;

                if (entry.is_directory(typeError)) {
                    a_callback(path);
                } else if (typeError) {
                    AddFilesystemIssue(a_result, path, "could not inspect entry in " + std::string{ a_label }, typeError);
                }

                iterator.increment(error);
                if (error) {
                    AddFilesystemIssue(a_result, a_parent, "could not continue enumerating " + std::string{ a_label }, error);
                    return;
                }
            }
        }
    }

    ViewDiscoveryResult DiscoverViews(const std::filesystem::path& a_viewsDirectory)
    {
        ViewDiscoveryResult result;

        std::error_code error;

        const bool viewsDirectoryExists = std::filesystem::is_directory(a_viewsDirectory, error);
        if (error) {
            AddFilesystemIssue(result, a_viewsDirectory, "could not inspect views directory", error);
            return result;
        }
        if (!viewsDirectoryExists) {
            result.issues.push_back({
                .path = a_viewsDirectory,
                .message = "views directory does not exist"
            });
            return result;
        }

        ForEachDirectory(result, a_viewsDirectory, "views directory", [&](const auto& a_modPath) {
            if (a_modPath.filename() == "shared") {
                return;
            }

            ForEachDirectory(result, a_modPath, "mod directory", [&](const auto& a_viewPath) {
                const auto manifestPath = a_viewPath / "manifest.json";
                std::error_code manifestError;
                const bool hasManifest = std::filesystem::is_regular_file(manifestPath, manifestError);

                if (manifestError) {
                    AddFilesystemIssue(result, manifestPath, "could not inspect manifest", manifestError);
                    return;
                }
                if (!hasManifest) {
                    return;
                }

                auto manifestResult = LoadViewManifest(manifestPath);
                if (!manifestResult) {
                    result.issues.push_back({
                        .path = manifestPath,
                        .message = manifestResult.error()
                    });
                    return;
                }

                result.views.push_back(std::move(*manifestResult));
            });
        });

        std::ranges::sort(result.views, {}, &ViewManifest::id);

        return result;
    }
}
