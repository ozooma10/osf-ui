#include "runtime/ViewManifest.h"

#include <array>

#include "core/Log.h"
#include "core/Version.h"
#include "runtime/Ids.h"
#include "runtime/Json.h"

namespace OSFUI
{
	namespace
	{
		// "<major>[.<minor>[.<patch>]]", digits only — missing parts are 0.
		bool ParseDottedVersion(std::string_view a_text, std::array<std::uint32_t, 3>& a_out)
		{
			a_out = {};
			std::size_t part = 0;
			const char* pos = a_text.data();
			const char* end = a_text.data() + a_text.size();
			while (part < 3) {
				const auto [next, ec] = std::from_chars(pos, end, a_out[part]);
				if (ec != std::errc{} || next == pos) {
					return false;
				}
				pos = next;
				++part;
				if (pos == end) {
					return true;
				}
				if (*pos != '.') {
					return false;
				}
				++pos;
			}
			return false;  // more than three parts (or trailing dot)
		}
	}

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
				{ "manifestVersion", "id", "title", "description", "hub", "entry",
					"width", "height", "transparent", "kind",
					"capturesInput", "pausesGame", "openOnStart", "order", "permissions",
					"targetVersion" },
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

		manifest.title = Json::GetString(*json, "title", manifest.id);
		manifest.description = Json::GetString(*json, "description", "");
		manifest.entry = Json::GetString(*json, "entry", manifest.entry);
		manifest.width = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
			Json::GetInt(*json, "width", manifest.width), 1, 16384));
		manifest.height = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
			Json::GetInt(*json, "height", manifest.height), 1, 16384));
		manifest.transparent = Json::GetBool(*json, "transparent", manifest.transparent);

		// Menu/HUD framework fields. Json has no enum helper, so `kind` is parsed manually; unknown values fall back to Menu
		const auto kindStr = Json::GetString(*json, "kind", "menu");
		manifest.kind = (kindStr == "hud") ? SurfaceKind::Hud : SurfaceKind::Menu;
		// `interactive` is derived, not author-facing: focus always follows the
		// top open menu (ApplyMenuPolicy), so menu ⇒ true, hud ⇒ false is the
		// only coherent mapping. (Was a manifest field pre-1.0; now ignored.)
		manifest.interactive = manifest.kind == SurfaceKind::Menu;
		manifest.capturesInput = Json::GetBool(*json, "capturesInput", manifest.capturesInput);
		manifest.pausesGame = Json::GetBool(*json, "pausesGame", manifest.pausesGame);
		manifest.openOnStart = Json::GetBool(*json, "openOnStart", manifest.openOnStart);
		manifest.order = static_cast<std::int32_t>(Json::GetInt(*json, "order", manifest.order));
		manifest.hub = Json::GetBool(*json, "hub", manifest.hub);

		// Advisory host-version target: never gates loading (a view authored
		// for a newer OSF UI still loads and does what it can — same lenient
		// stance as unknown keys), but the catalog carries it so the Mods
		// surface can badge "needs update", and the log records it for triage.
		if (auto target = Json::GetString(*json, "targetVersion", ""); !target.empty()) {
			std::array<std::uint32_t, 3> targetParts{};
			if (ParseDottedVersion(target, targetParts)) {
				manifest.targetVersion = std::move(target);
				constexpr std::array<std::uint32_t, 3> hostParts{
					kPluginVersionMajor, kPluginVersionMinor, kPluginVersionPatch
				};
				if (hostParts < targetParts) {
					REX::WARN("ViewManifest: view '{}' targets OSF UI {} but this is {} — update OSF UI",
						manifest.id, manifest.targetVersion, kPluginVersion);
				}
			} else {
				REX::WARN("ViewManifest: {} targetVersion '{}' is not '<major>[.<minor>[.<patch>]]' — ignored",
					a_path.string(), target);
			}
		}

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
		}

		return manifest;
	}
}
