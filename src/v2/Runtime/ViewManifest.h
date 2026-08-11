#pragma once

namespace Runtime
{
    struct ViewManifest
    {
        std::string id;

        std::string title;
        std::string entry{ "index.html" };
        std::uint32_t width{ 1600 };
        std::uint32_t height{ 900 };
        bool transparent{ true };

        std::filesystem::path rootDirectory;
    };

    using ManifestResult = std::expected<ViewManifest, std::string>;
    ManifestResult LoadViewManifest(const std::filesystem::path& a_manifestPath);
}