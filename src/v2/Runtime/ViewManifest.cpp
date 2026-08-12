#include "ViewManifest.h"

#include <glaze/glaze.hpp>

namespace Runtime
{
    // Glaze reflection requires external linkage, so this cannot live in the anonymous namespace.
    struct ManifestJson
    {
        std::optional<std::string> title;
        std::string entry{ "index.html" };
        std::int64_t width{ 1600 };
        std::int64_t height{ 900 };
        bool transparent{ true };

        std::string kind{ "menu" };
        bool capturesInput{ true };
        bool pausesGame{ true };
        bool openOnStart{ false };
    };

    namespace
    {
        inline constexpr auto kManifestOptions = glz::opts{
            .comments = true,
            .error_on_unknown_keys = false,
        };

        ManifestResult Failure(std::string a_message)
        {
            return std::unexpected<std::string>{ std::move(a_message) };
        }

        bool IsValidSegment(std::string_view a_value)
        {
            if (a_value.empty()) { return false; }
            for (const char character : a_value) {
                if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-')) {
                    return false;
                }
            }
            return true;
        }
    
        bool IsValidModId(std::string_view a_modId)
        {
            if (a_modId == "osfui") { return true; } //osfui allows no dots
            if (a_modId.empty()) { return false; }

            const auto firstDot = a_modId.find('.');

            if (firstDot == std::string_view::npos) { return false; }

            if (a_modId.find('.', firstDot + 1) != std::string_view::npos) {
                return false;
            }

            return IsValidSegment(a_modId.substr(0, firstDot)) && IsValidSegment(a_modId.substr(firstDot + 1));
        }
    
        bool IsValidViewName(std::string_view a_viewName)
        {
            return IsValidSegment(a_viewName);
        }

        bool IsSafeEntry(std::string_view a_entry)
        {
            const std::filesystem::path entryPath{ a_entry };
            if (entryPath.empty() || entryPath.is_absolute() || entryPath.has_root_name() || entryPath.has_root_directory()) {
                return false;
            }
            // Prevent entries such as "../other-view/index.html".
            for (const auto& component : entryPath) {
                if (component == "..") {
                    return false;
                }
            }
            return true;
        }

        ViewKind ParseViewKind(std::string_view a_value, const std::filesystem::path& a_manifestPath)
        {
            if(a_value == "hud") { return ViewKind::Hud; }
            return ViewKind::Menu; // Default to Menu if the kind is unrecognized.
        }
    }

    ManifestResult LoadViewManifest(const std::filesystem::path& a_manifestPath)
    {
        std::ifstream input{a_manifestPath, std::ios::binary};
        if(!input.is_open()) {
            return Failure("could not open manifest '" + a_manifestPath.string() + "'");
        }

        std::string buffer{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        if(input.bad()) {
            return Failure("failed while reading manifest '" + a_manifestPath.string() + "'");
        }

        ManifestJson parsed{};
        if(const auto error = glz::read<kManifestOptions>(parsed, buffer)) {
            return Failure("failed to parse manifest '" + a_manifestPath.string() + "': " + glz::format_error(error, buffer));
        }

        //Expectedlayout is Views/author.mod/inventory/view.json.(viewname=inventory, modId=author.mod)
        const auto viewDirectory = a_manifestPath.parent_path();
        const auto viewName = viewDirectory.filename().string();
        const auto modId = viewDirectory.parent_path().filename().string();

        if(!IsValidModId(modId)) {
            return Failure("invalid mod ID '" + modId + "' in manifest path '" + a_manifestPath.string() + "'");
        }
        if(!IsValidViewName(viewName)) {
            return Failure("invalid view name '" + viewName + "' in manifest path '" + a_manifestPath.string() + "'");
        }
        if(!IsSafeEntry(parsed.entry)) {
            return Failure("invalid entry '" + parsed.entry + "' in manifest path '" + a_manifestPath.string() + "'");
        }

        const auto kind = ParseViewKind(parsed.kind, a_manifestPath);
        const bool capturesInput = kind == ViewKind::Menu && parsed.capturesInput;
        const bool pausesGame = kind == ViewKind::Menu && parsed.pausesGame;

        ViewManifest manifest {
            .id = modId + "/" + viewName,
            .title = parsed.title.value_or(viewName),
            .entry = std::move(parsed.entry),
            .width = static_cast<std::uint32_t>(std::clamp<std::int64_t>(parsed.width, 1, 16384)),
            .height = static_cast<std::uint32_t>(std::clamp<std::int64_t>(parsed.height, 1, 16384)),
            .transparent = parsed.transparent,
            .kind = kind,
            .capturesInput = capturesInput,
            .pausesGame = pausesGame,
            .rootDirectory = std::move(viewDirectory)
        };
        return manifest;
    }
}
