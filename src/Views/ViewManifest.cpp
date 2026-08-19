#include "Views/ViewManifest.h"

#include <array>

#include "Core/Log.h"
#include "Core/Version.h"
#include "Core/Ids.h"
#include "Core/Json.h"

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
		Json::CheckFormatVersion(*json, "manifestVersion", 1, "ViewManifest: [content] " + a_path.string());
		if (Log::DebugEnabled()) {
			Json::ReportUnknownKeys(*json,
				{ "manifestVersion", "mod", "title", "description", "hub", "debugOnly", "entry",
					"width", "height", "transparent", "kind",
					"capturesInput", "pausesGame", "openOnStart", "order",
					"targetVersion" },
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

		// Ignore declared id; retain mod only as an authoring consistency check.

		manifest.title = Json::Get(*json, "title", manifest.id);
		manifest.description = Json::Get(*json, "description", "");
		manifest.entry = Json::Get(*json, "entry", manifest.entry);
		manifest.width = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
			Json::Get(*json, "width", manifest.width), 1, 16384));
		manifest.height = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
			Json::Get(*json, "height", manifest.height), 1, 16384));
		manifest.transparent = Json::Get(*json, "transparent", manifest.transparent);

		// Json has no enum helper, so `kind` is parsed manually; unknown values fall back to Menu.
		const auto kindStr = Json::Get(*json, "kind", "menu");
		manifest.kind = (kindStr == "hud") ? ViewKind::Hud : ViewKind::Menu;
		// Derive interactivity from active-menu policy; ignore the pre-1.0 manifest field.
		manifest.menuInputEligible = manifest.kind == ViewKind::Menu;
		manifest.capturesInput = Json::Get(*json, "capturesInput", manifest.capturesInput);
		manifest.pausesGame = Json::Get(*json, "pausesGame", manifest.pausesGame);
		manifest.openOnStart = Json::Get(*json, "openOnStart", manifest.openOnStart);
		manifest.order = static_cast<std::int32_t>(Json::Get(*json, "order", manifest.order));
		manifest.catalogVisible = Json::Get(*json, "hub", manifest.catalogVisible);
		manifest.debugOnly = Json::Get(*json, "debugOnly", manifest.debugOnly);

		// Retain pre-2.0 targets only to select the frozen v1 compatibility facade.
		if (auto target = Json::Get(*json, "targetVersion", ""); !target.empty()) {
			if (const auto targetParts = ParseDottedVersion(target)) {
				manifest.targetVersion = std::move(target);
				if (kOsfuiReleaseVersionParts < *targetParts) {
					REX::WARN("ViewManifest: [content] view '{}' targets OSF UI {} but this is {} — update OSF UI",
						manifest.id, manifest.targetVersion, kOsfuiReleaseVersion);
				}
			} else {
				REX::WARN("ViewManifest: [content] {} targetVersion '{}' is not '<major>[.<minor>[.<patch>]]' — ignored",
					a_path.string(), target);
			}
		}

		// Reject entry paths that escape the view's asset folder.
		const auto entryPath = std::filesystem::path(manifest.entry);
		if (entryPath.is_absolute() ||
			std::ranges::any_of(entryPath, [](const auto& part) { return part == ".."; })) {
			REX::ERROR("ViewManifest: [content] {} entry '{}' must be a relative path inside the view folder",
				a_path.string(), manifest.entry);
			return std::nullopt;
		}

		// Force HUDs passive so malformed manifests cannot capture input or pause.
		if (manifest.kind == ViewKind::Hud) {
			if (manifest.capturesInput || manifest.pausesGame) {
				REX::WARN("ViewManifest: [content] HUD '{}' cannot capture input or pause; forcing both off", manifest.id);
			}
			manifest.capturesInput = false;
			manifest.pausesGame = false;
		}

		return manifest;
	}
}
