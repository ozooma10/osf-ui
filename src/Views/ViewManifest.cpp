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

		// `manifestVersion` is accepted but not required — the nested
		// views/<modId>/<viewName>/ layout is itself the v2 discriminator. Unknown keys
		// are the normal compatible case (a newer mod on an older OSF UI release), so they
		// report as developer-mode INFO, never a warning.
		Json::CheckFormatVersion(*json, "manifestVersion", 1, "ViewManifest: [content] " + a_path.string());
		if (Log::DevMode()) {
			Json::ReportUnknownKeys(*json,
				{ "manifestVersion", "mod", "title", "description", "hub", "debugOnly", "entry",
					"width", "height", "transparent", "kind",
					"capturesInput", "pausesGame", "openOnStart", "order", "permissions",
					"targetVersion" },
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

		// A declared `id` is ignored and no longer accepted: the folder name
		// already is the id, so one reports as an unknown key in dev mode like
		// any other field this runtime does not read. `mod` stays accepted
		// though equally underived-from — the frontend build cross-checks it
		// against the source directory (frontend/scripts/config.mjs), which
		// catches a manifest copied into the wrong folder at authoring time.

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
		// `interactive` is derived, not author-facing: focus eligibility follows the active
		// menu (ApplyViewPresentationPolicy), so menu => true, hud => false. Was a manifest
		// field pre-1.0; now ignored.
		manifest.menuInputEligible = manifest.kind == ViewKind::Menu;
		manifest.capturesInput = Json::Get(*json, "capturesInput", manifest.capturesInput);
		manifest.pausesGame = Json::Get(*json, "pausesGame", manifest.pausesGame);
		manifest.openOnStart = Json::Get(*json, "openOnStart", manifest.openOnStart);
		manifest.order = static_cast<std::int32_t>(Json::Get(*json, "order", manifest.order));
		manifest.catalogVisible = Json::Get(*json, "hub", manifest.catalogVisible);
		manifest.debugOnly = Json::Get(*json, "debugOnly", manifest.debugOnly);

		// A newer target remains advisory and badges "needs update". An explicitly
		// pre-2.0 target is retained so the temporary v1 navigation façade and its
		// persistent 2.1.0 removal warning can be selected deterministically.
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

		if (const auto* permissions = Json::GetObject(*json, "permissions")) {
			manifest.permissions.nativeBridge = Json::Get(*permissions, "nativeBridge", false);
			manifest.permissions.filesystem = Json::Get(*permissions, "filesystem", false);
			manifest.permissions.network = Json::Get(*permissions, "network", false);
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
