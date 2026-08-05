#include "runtime/ViewManifest.h"

#include <array>

#include "core/Log.h"
#include "core/Color.h"
#include "core/Version.h"
#include "runtime/Ids.h"
#include "runtime/Json.h"

namespace OSFUI
{
	std::optional<ViewManifest> ViewManifest::Load(const std::filesystem::path& a_path)
	{
		const auto json = Json::ParseFile(a_path);
		if (!json || !json->is_object()) {
			REX::ERROR("ViewManifest: [content] {} is not a valid JSON object", a_path.string());
			return std::nullopt;
		}

		// `manifestVersion` is accepted but not required — the nested
		// views/<mod>/<view>/ layout is itself the v2 discriminator. Unknown keys
		// are the normal compatible case (a newer mod on an older host), so they
		// surface as devMode INFO, never a warning.
		Json::CheckFormatVersion(*json, "manifestVersion", 1, "ViewManifest: [content] " + a_path.string());
		if (Log::DevMode()) {
			Json::ReportUnknownKeys(*json,
				{ "manifestVersion", "id", "title", "description", "accent", "hub", "debugOnly", "entry",
					"width", "height", "transparent", "kind",
					"capturesInput", "pausesGame", "openOnStart", "order", "readySignal", "permissions",
					"targetVersion", "papyrus" },
				"ViewManifest: [content] " + a_path.string(), /*a_warn=*/false);
		}

		// The path is the identity: the manifest lives at
		// views/<modId>/<viewName>/manifest.json and the qualified view id is
		// "<modId>/<viewName>". Declared fields are consistency checks, not
		// sources of truth — a manifest can't claim another mod's namespace.
		const auto viewName = a_path.parent_path().filename().string();
		const auto modId = a_path.parent_path().parent_path().filename().string();
		if (!Ids::IsAcceptedModId(modId) || !Ids::IsValidViewName(viewName)) {
			REX::ERROR("ViewManifest: [content] {} — views live at views/<author>.<modname>/<view>/manifest.json "
					   "(lowercase [a-z0-9-] segments; dotless mod folders are reserved for the platform)",
				a_path.string());
			return std::nullopt;
		}

		ViewManifest manifest;
		manifest.rootDir = a_path.parent_path();
		manifest.id = modId + "/" + viewName;
		manifest.mod = modId;

		// A declared `id` is ignored: the folder name already is the id.
		// It stays in the accepted-keys list so pre-existing manifests
		// don't report it as unknown.

		manifest.title = Json::GetString(*json, "title", manifest.id);
		manifest.description = Json::GetString(*json, "description", "");
		if (auto accent = Json::GetString(*json, "accent", ""); !accent.empty()) {
			if (IsHexColor(accent)) {
				std::ranges::transform(accent, accent.begin(), [](char c) {
					return (c >= 'A' && c <= 'F') ? static_cast<char>(c + 32) : c;
				});
				manifest.accent = std::move(accent);
			} else {
				REX::WARN("ViewManifest: [content] view '{}' accent '{}' is not #rrggbb or #rrggbbaa — ignored", manifest.id, accent);
			}
		}
		manifest.entry = Json::GetString(*json, "entry", manifest.entry);
		manifest.width = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
			Json::GetInt(*json, "width", manifest.width), 1, 16384));
		manifest.height = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
			Json::GetInt(*json, "height", manifest.height), 1, 16384));
		manifest.transparent = Json::GetBool(*json, "transparent", manifest.transparent);

		// Json has no enum helper, so `kind` is parsed manually; unknown values fall back to Menu.
		const auto kindStr = Json::GetString(*json, "kind", "menu");
		manifest.kind = (kindStr == "hud") ? SurfaceKind::Hud : SurfaceKind::Menu;
		// `interactive` is derived, not author-facing: focus follows the top open
		// menu (ApplyMenuPolicy), so menu => true, hud => false. Was a manifest
		// field pre-1.0; now ignored.
		manifest.interactive = manifest.kind == SurfaceKind::Menu;
		manifest.capturesInput = Json::GetBool(*json, "capturesInput", manifest.capturesInput);
		manifest.pausesGame = Json::GetBool(*json, "pausesGame", manifest.pausesGame);
		manifest.openOnStart = Json::GetBool(*json, "openOnStart", manifest.openOnStart);
		manifest.order = static_cast<std::int32_t>(Json::GetInt(*json, "order", manifest.order));
		manifest.hub = Json::GetBool(*json, "hub", manifest.hub);
		manifest.debugOnly = Json::GetBool(*json, "debugOnly", manifest.debugOnly);
		manifest.readySignal = Json::GetBool(*json, "readySignal", manifest.readySignal);

		// A newer target remains advisory and badges "needs update". An explicitly
		// pre-2.0 target is retained so Runtime can refuse it with a legible System
		// Health condition instead of navigating into the removed helper API.
		if (auto target = Json::GetString(*json, "targetVersion", ""); !target.empty()) {
			std::array<std::uint32_t, 3> targetParts{};
			if (ParseDottedVersion(target, targetParts)) {
				manifest.targetVersion = std::move(target);
				if (kPluginVersionParts < targetParts) {
					REX::WARN("ViewManifest: [content] view '{}' targets OSF UI {} but this is {} — update OSF UI",
						manifest.id, manifest.targetVersion, kPluginVersion);
				}
			} else {
				REX::WARN("ViewManifest: [content] {} targetVersion '{}' is not '<major>[.<minor>[.<patch>]]' — ignored",
					a_path.string(), target);
			}
		}

		if (const auto it = json->find("permissions"); it != json->end() && it->is_object()) {
			manifest.permissions.nativeBridge = Json::GetBool(*it, "nativeBridge", false);
			manifest.permissions.filesystem = Json::GetBool(*it, "filesystem", false);
			manifest.permissions.network = Json::GetBool(*it, "network", false);
		}
		if (manifest.readySignal && !manifest.permissions.nativeBridge) {
			REX::WARN("ViewManifest: [content] view '{}' requests readySignal without nativeBridge; using load completion", manifest.id);
			manifest.readySignal = false;
		}

		// Views may only reference their own local assets; reject entries that
		// escape the view folder.
		const auto entryPath = std::filesystem::path(manifest.entry);
		if (entryPath.is_absolute() ||
			std::ranges::any_of(entryPath, [](const auto& part) { return part == ".."; })) {
			REX::ERROR("ViewManifest: [content] {} entry '{}' must be a relative path inside the view folder",
				a_path.string(), manifest.entry);
			return std::nullopt;
		}

		if (manifest.permissions.network) {
			REX::WARN("ViewManifest: [content] view '{}' requests network permission; not supported, forcing off", manifest.id);
			manifest.permissions.network = false;
		}

		// A HUD is passive: it draws over live gameplay but never captures input,
		// pauses, or becomes the focused view. Forced here so a mis-authored
		// manifest can't create a HUD that steals input.
		if (manifest.kind == SurfaceKind::Hud) {
			if (manifest.capturesInput || manifest.pausesGame) {
				REX::WARN("ViewManifest: [content] HUD '{}' cannot capture input or pause; forcing both off", manifest.id);
			}
			manifest.capturesInput = false;
			manifest.pausesGame = false;
		}

		return manifest;
	}
}
