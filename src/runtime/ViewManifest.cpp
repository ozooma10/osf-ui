#include "runtime/ViewManifest.h"

#include "core/Log.h"
#include "runtime/Ids.h"
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

		// Format bookkeeping (api-freeze-plan item 8). `manifestVersion` is
		// accepted but not required — the nested views/<mod>/<view>/ layout
		// (item 1) is itself the v2 discriminator. Unknown keys are the NORMAL
		// compatible case for author-shipped files (a newer mod on an older
		// host), so they surface as devMode INFO, never a warning.
		if (const auto v = Json::GetInt(*json, "manifestVersion", 1); v > 1) {
			REX::INFO("ViewManifest: {} declares manifestVersion {} — authored for a newer OSF UI; unknown fields are ignored",
				a_path.string(), v);
		}
		if (Log::DevMode()) {
			Json::ReportUnknownKeys(*json,
				{ "manifestVersion", "id", "mod", "title", "description", "hub", "entry",
					"width", "height", "transparent", "zorder", "interactive", "kind",
					"capturesInput", "pausesGame", "openOnStart", "order", "permissions" },
				"ViewManifest: " + a_path.string(), /*a_warn=*/false);
		}

		// The path IS the identity (api-freeze-plan item 1): the manifest lives
		// at views/<modId>/<viewName>/manifest.json, and the qualified view id
		// is "<modId>/<viewName>". Declared fields are consistency checks, not
		// sources of truth — a manifest can't claim another mod's namespace.
		const auto viewName = a_path.parent_path().filename().string();
		const auto modId = a_path.parent_path().parent_path().filename().string();
		if (!Ids::IsAcceptedModId(modId) || !Ids::IsValidViewName(viewName)) {
			REX::ERROR("ViewManifest: {} — views live at views/<author>.<modname>/<view>/manifest.json "
					   "(lowercase [a-z0-9-] segments; dotless mod folders are reserved for the platform)",
				a_path.string());
			return std::nullopt;
		}

		ViewManifest manifest;
		manifest.rootDir = a_path.parent_path();
		manifest.id = modId + "/" + viewName;
		manifest.mod = modId;

		// Required `id` must equal the view folder name — a mismatch is a
		// hard reject (a copied manifest that forgot the rename).
		const auto declaredId = Json::GetString(*json, "id", "");
		if (declaredId != viewName) {
			REX::ERROR("ViewManifest: {} declares id '{}' but the view folder is '{}' — "
					   "'id' must equal the folder name",
				a_path.string(), declaredId, viewName);
			return std::nullopt;
		}
		// Optional `mod` is derivable from the path; when declared it must match.
		if (const auto declaredMod = Json::GetString(*json, "mod", ""); !declaredMod.empty() && declaredMod != modId) {
			REX::ERROR("ViewManifest: {} declares mod '{}' but lives under views/{}/ — "
					   "'mod' is optional and must equal the mod folder when present",
				a_path.string(), declaredMod, modId);
			return std::nullopt;
		}

		manifest.title = Json::GetString(*json, "title", manifest.id);
		manifest.description = Json::GetString(*json, "description", "");
		manifest.entry = Json::GetString(*json, "entry", manifest.entry);
		manifest.width = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
			Json::GetInt(*json, "width", manifest.width), 1, 16384));
		manifest.height = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
			Json::GetInt(*json, "height", manifest.height), 1, 16384));
		manifest.transparent = Json::GetBool(*json, "transparent", manifest.transparent);
		manifest.zorder = static_cast<std::int32_t>(Json::GetInt(*json, "zorder", manifest.zorder));
		manifest.interactive = Json::GetBool(*json, "interactive", manifest.interactive);

		// Menu/HUD framework fields. Json has no enum helper, so `kind` is parsed manually; unknown values fall back to Menu
		const auto kindStr = Json::GetString(*json, "kind", "menu");
		manifest.kind = (kindStr == "hud") ? SurfaceKind::Hud : SurfaceKind::Menu;
		manifest.capturesInput = Json::GetBool(*json, "capturesInput", manifest.capturesInput);
		manifest.pausesGame = Json::GetBool(*json, "pausesGame", manifest.pausesGame);
		manifest.openOnStart = Json::GetBool(*json, "openOnStart", manifest.openOnStart);
		manifest.order = static_cast<std::int32_t>(Json::GetInt(*json, "order", manifest.order));
		manifest.hub = Json::GetBool(*json, "hub", manifest.hub);

		if (const auto it = json->find("permissions"); it != json->end() && it->is_object()) {
			manifest.permissions.nativeBridge = Json::GetBool(*it, "nativeBridge", false);
			manifest.permissions.filesystem = Json::GetBool(*it, "filesystem", false);
			manifest.permissions.network = Json::GetBool(*it, "network", false);
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

		// A HUD is passive by definition: it draws live over gameplay but never captures input, pauses, or becomes the focused/active view
		// Force the invariants so a mis-authored manifest can't create a HUD that teals input.
		if (manifest.kind == SurfaceKind::Hud) {
			if (manifest.capturesInput || manifest.pausesGame) {
				REX::WARN("ViewManifest: HUD '{}' cannot capture input or pause; forcing both off", manifest.id);
			}
			manifest.capturesInput = false;
			manifest.pausesGame = false;
			manifest.interactive = false;
		}

		return manifest;
	}
}
