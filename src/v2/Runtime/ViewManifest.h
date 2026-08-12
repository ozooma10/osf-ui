#pragma once

namespace Runtime
{
    enum class ViewKind
    {
        Menu,
        Hud
    };

    struct ViewManifest
    {
        std::string id;

        std::string title;
        std::string entry{ "index.html" };
        std::uint32_t width{ 1600 };
        std::uint32_t height{ 900 };
        bool transparent{ true };

        ViewKind kind{ ViewKind::Menu };
        bool capturesInput{ true };
        bool pausesGame{ true };
        bool openOnStart{ false };

        std::filesystem::path rootDirectory;
    };

    using ManifestResult = std::expected<ViewManifest, std::string>;
    ManifestResult LoadViewManifest(const std::filesystem::path& a_manifestPath);
}