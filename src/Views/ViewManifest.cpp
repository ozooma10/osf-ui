#include "Views/ViewManifest.h"

#include <array>

#include "Core/Log.h"
#include "Core/Ids.h"
#include "Core/Json.h"
#include "World/WorldAssets.h"

namespace OSFUI
{
	std::optional<ViewManifest> ViewManifest::Load(const std::filesystem::path& a_path)
	{
		const auto json = Json::ParseFile(a_path);
		if (!json || !json->is_object()) {
			REX::ERROR("ViewManifest: [content] {} is not a valid JSON object", a_path.string());
			return std::nullopt;
		}

		// Nested paths identify v2; unknown keys remain forward-compatible developer INFO.
		const auto versionIt = json->find("manifestVersion");
		if (versionIt == json->end() || !versionIt->is_number_integer() || versionIt->get<std::int64_t>() != 1) {
			REX::ERROR("ViewManifest: [content] {} requires manifestVersion 1", a_path.string());
			return std::nullopt;
		}
		if (Log::DebugEnabled()) {
			Json::ReportUnknownKeys(*json,
				{ "manifestVersion", "mod", "title", "description", "debugOnly", "entry",
					"width", "height", "transparent", "kind", "texture",
					"capturesInput", "pausesGame", "openOnStart", "order" },
				"ViewManifest: [content] " + a_path.string(), /*a_warn=*/false);
		}

		// Derive view identity from views/<modId>/<viewName>, never declared fields.
		const auto viewName = a_path.parent_path().filename().string();
		const auto modId = a_path.parent_path().parent_path().filename().string();
		if (!Ids::IsAcceptedModId(modId) || !Ids::IsValidViewName(viewName)) {
			REX::ERROR("ViewManifest: [content] {} — views live at views/<modId>/<view>/manifest.json with a safe mod-id folder",
				a_path.string());
			return std::nullopt;
		}

		ViewManifest manifest;
		manifest.rootDir = a_path.parent_path();
		manifest.id = modId + "/" + viewName;
		manifest.mod = modId;
		if (!Ids::IsValidQualifiedViewId(manifest.id)) {
			REX::ERROR("ViewManifest: [content] '{}' is a removed reserved OSF UI view id", manifest.id);
			return std::nullopt;
		}

		// Ignore declared id; retain mod only as an authoring consistency check.

		manifest.title = Json::Get(*json, "title", manifest.id);
		manifest.description = Json::Get(*json, "description", "");
		manifest.entry = Json::Get(*json, "entry", manifest.entry);
		manifest.width = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
			Json::Get(*json, "width", manifest.width), 1, 16384));
		manifest.height = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
			Json::Get(*json, "height", manifest.height), 1, 16384));
		manifest.transparent = Json::Get(*json, "transparent", manifest.transparent);

		// Unknown kinds are malformed rather than silently gaining menu/input privileges.
		const auto kindStr = Json::Get(*json, "kind", "menu");
		if (kindStr != "menu" && kindStr != "hud" && kindStr != "world") {
			REX::ERROR("ViewManifest: [content] {} kind '{}' must be 'menu', 'hud', or 'world'", a_path.string(), kindStr);
			return std::nullopt;
		}
		manifest.kind = kindStr == "world" ? ViewKind::World :
			(kindStr == "hud" ? ViewKind::Hud : ViewKind::Menu);
		// Derive interactivity from active-menu policy; ignore the pre-1.0 manifest field.
		manifest.menuInputEligible = manifest.kind == ViewKind::Menu;
		manifest.capturesInput = Json::Get(*json, "capturesInput", manifest.capturesInput);
		manifest.pausesGame = Json::Get(*json, "pausesGame", manifest.pausesGame);
		manifest.openOnStart = Json::Get(*json, "openOnStart", manifest.openOnStart);
		manifest.order = static_cast<std::int32_t>(Json::Get(*json, "order", manifest.order));
		manifest.debugOnly = Json::Get(*json, "debugOnly", manifest.debugOnly);

		if (manifest.kind == ViewKind::World) {
			// Validate the authored integers before narrowing/clamping. A wrapped
			// value must never overflow output allocation.
			const auto readSize = [&](std::string_view a_key, std::uint32_t a_default,
				std::uint32_t a_minimum) -> std::optional<std::uint32_t> {
				const auto it = json->find(a_key);
				if (it == json->end()) {
					return a_default >= a_minimum ? std::optional(a_default) : std::nullopt;
				}
				if (!it->is_number_integer() ||
					(!it->is_number_unsigned() && it->get<std::int64_t>() < 0)) {
					return std::nullopt;
				}
				const auto value = it->get<std::uint64_t>();
				return value >= a_minimum && value <= 4096 ?
					std::optional(static_cast<std::uint32_t>(value)) : std::nullopt;
			};
			const auto width = readSize("width", kDefaultViewWidth, 1);
			const auto height = readSize("height", kDefaultViewHeight, 1);
			const auto texture = Json::Get(*json, "texture", "");
			if (!width || !height || !WorldAssets::IsGeneratedPath(texture) || json->contains("placeholderSize")) {
				REX::ERROR("ViewManifest: [content] world '{}' requires width/height in 1..4096 and a generated texture binding; rebuild old placeholderSize packages", manifest.id);
				return std::nullopt;
			}
			manifest.width = *width;
			manifest.height = *height;
			manifest.texture = texture;
			manifest.transparent = false;
			manifest.openOnStart = false;
			manifest.order = 0;
		}

		// Reject entry paths that escape the view's asset folder.
		const auto entryPath = std::filesystem::path(manifest.entry);
		if (entryPath.is_absolute() ||
			std::ranges::any_of(entryPath, [](const auto& part) { return part == ".."; })) {
			REX::ERROR("ViewManifest: [content] {} entry '{}' must be a relative path inside the view folder",
				a_path.string(), manifest.entry);
			return std::nullopt;
		}

		// Passive views cannot acquire fullscreen input or pause ownership.
		if (manifest.kind != ViewKind::Menu) {
			if (manifest.capturesInput || manifest.pausesGame) {
				REX::WARN("ViewManifest: [content] {} view '{}' cannot capture input or pause; forcing both off", kindStr, manifest.id);
			}
			manifest.capturesInput = false;
			manifest.pausesGame = false;
		}

		return manifest;
	}
}
