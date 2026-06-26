#include "runtime/ViewManifest.h"

#include "runtime/Json.h"

namespace OSFUI
{
	std::optional<ViewManifest> ViewManifest::Load(const std::filesystem::path& a_path)
	{
		const auto json = Json::ParseFile(a_path);
		if (!json || !json->is_object()) {
			REX::ERROR("ViewManifest: {} is not a valid JSON object", a_path.string());
			return std::nullopt;
		}

		ViewManifest manifest;
		manifest.rootDir = a_path.parent_path();
		manifest.id = Json::GetString(*json, "id", "");
		manifest.title = Json::GetString(*json, "title", manifest.id);
		manifest.entry = Json::GetString(*json, "entry", manifest.entry);
		manifest.width = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
			Json::GetInt(*json, "width", manifest.width), 1, 16384));
		manifest.height = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
			Json::GetInt(*json, "height", manifest.height), 1, 16384));
		manifest.transparent = Json::GetBool(*json, "transparent", manifest.transparent);
		manifest.zorder = static_cast<std::int32_t>(Json::GetInt(*json, "zorder", manifest.zorder));
		manifest.interactive = Json::GetBool(*json, "interactive", manifest.interactive);

		if (const auto it = json->find("permissions"); it != json->end() && it->is_object()) {
			manifest.permissions.nativeBridge = Json::GetBool(*it, "nativeBridge", false);
			manifest.permissions.filesystem = Json::GetBool(*it, "filesystem", false);
			manifest.permissions.network = Json::GetBool(*it, "network", false);
		}

		if (manifest.id.empty()) {
			REX::ERROR("ViewManifest: {} is missing required field 'id'", a_path.string());
			return std::nullopt;
		}

		// Reject entries that try to escape the view folder. Views may only
		// reference their own local assets.
		const auto entryPath = std::filesystem::path(manifest.entry);
		if (entryPath.is_absolute() ||
			std::ranges::any_of(entryPath, [](const auto& part) { return part == ".."; })) {
			REX::ERROR("ViewManifest: {} entry '{}' must be a relative path inside the view folder",
				a_path.string(), manifest.entry);
			return std::nullopt;
		}

		if (manifest.permissions.network) {
			REX::WARN("ViewManifest: view '{}' requests network permission; not supported, forcing off", manifest.id);
			manifest.permissions.network = false;
		}

		return manifest;
	}
}
