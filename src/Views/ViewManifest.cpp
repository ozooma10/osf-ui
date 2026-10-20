#include "Views/ViewManifest.h"

#include <array>

#include "Core/Log.h"
#include "Core/Ids.h"
#include "Core/Json.h"
#include "Core/Utf8Path.h"
#include "BrowserSurface.h"

namespace OSFUI
{
	std::optional<ViewManifest> ViewManifest::Load(const std::filesystem::path& a_path)
	{
		const auto json = Json::ParseFile(a_path);
		return json ? Parse(a_path, *json) : std::nullopt;
	}

	std::optional<ViewManifest> ViewManifest::Parse(const std::filesystem::path& a_path, const nlohmann::json& a_document)
	{
		// View IDs come from folder names, not manifest fields.
		const auto viewName = Utf8Path(a_path.parent_path().filename());
		const auto modId = Utf8Path(a_path.parent_path().parent_path().filename());
		if (!Ids::IsAcceptedModId(modId) || !Ids::IsValidViewName(viewName)) {
			REX::ERROR("ViewManifest: [content] {} — views live at views/<modId>/<view>/manifest.json; mod folders must use URL-safe ASCII and view folders must use a-z, 0-9, '-'", Utf8Path(a_path));
			return std::nullopt;
		}

		const auto* json = &a_document;
		if (!json->is_object()) {
			REX::ERROR("ViewManifest: [content] {} is not a valid JSON object", Utf8Path(a_path));
			return std::nullopt;
		}

		const auto versionIt = json->find("manifestVersion");
		if (versionIt == json->end() || !versionIt->is_number_integer() || versionIt->get<std::int64_t>() != 1) {
			REX::ERROR("ViewManifest: [content] {} requires manifestVersion 1", Utf8Path(a_path));
			return std::nullopt;
		}
		// Unknown keys are INFO-only for forward compatibility.
		if (Log::DebugEnabled()) {
			Json::ReportUnknownKeys(*json,
				{ "manifestVersion", "mod", "title", "description", "debugOnly", "entry", "launcher",
					"width", "height", "transparent", "kind", "texture", "fullscreen", "browser",
					"capturesInput", "pausesGame", "openOnStart", "order" },
				"ViewManifest: [content] " + Utf8Path(a_path), /*a_warn=*/false);
		}

		ViewManifest manifest;
		manifest.id = modId + "/" + viewName;
		manifest.mod = modId;
		if (!Ids::IsValidQualifiedViewId(manifest.id)) {
			REX::ERROR("ViewManifest: [content] '{}' is a removed reserved OSF UI view id", manifest.id);
			return std::nullopt;
		}

		manifest.title = Json::Get(*json, "title", manifest.id);
		manifest.description = Json::Get(*json, "description", "");
		manifest.entry = Json::Get(*json, "entry", manifest.entry);
		manifest.width = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
			Json::Get(*json, "width", manifest.width), 1, 16384));
		manifest.height = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
			Json::Get(*json, "height", manifest.height), 1, 16384));

		const auto kindStr = Json::Get(*json, "kind", "menu");
		if (kindStr != "menu" && kindStr != "hud" && kindStr != "world") {
			REX::ERROR("ViewManifest: [content] {} kind '{}' must be 'menu', 'hud', or 'world'", Utf8Path(a_path), kindStr);
			return std::nullopt;
		}
		manifest.kind = kindStr == "world" ? ViewKind::Surface : (kindStr == "hud" ? ViewKind::Hud : ViewKind::Menu);
		manifest.texture = Json::Get(*json, "texture", "");
		if (manifest.kind == ViewKind::Surface && (manifest.texture.empty() || manifest.width > 4096 || manifest.height > 4096)) {
            REX::ERROR("ViewManifest: '{}' world views require a texture and dimensions at most 4096", manifest.id);
            return std::nullopt;
        }
		manifest.fullscreen = Json::Get(*json, "fullscreen", false);
		if (const auto browser = json->find("browser"); browser != json->end()) {
			if (manifest.id != osfui::browser::kBrowserView || manifest.kind != ViewKind::Surface ||
				!browser->is_object() || !browser->contains("chrome") || !(*browser)["chrome"].is_string() ||
				(browser->contains("chromeHeight") && !(*browser)["chromeHeight"].is_number_integer())) {
				REX::ERROR("ViewManifest: browser requires the reserved osfui.browser/tv world view");
				return std::nullopt;
			}
			manifest.browserChrome = (*browser)["chrome"].get<std::string>();
			const auto band = Json::Get(*browser, "chromeHeight", 56);
			const auto& chrome = manifest.browserChrome;
			if (chrome.empty() || chrome.front() == '/' || chrome.find_first_of("\\:?#%") != chrome.npos ||
				chrome.find("..") != chrome.npos || band < 32 || band > 256 || static_cast<std::uint32_t>(band) >= manifest.height) return std::nullopt;
			manifest.browserChromeHeight = static_cast<std::uint32_t>(band);
			manifest.entry = chrome;
			manifest.fullscreen = true;
		}
		if (manifest.fullscreen && manifest.kind != ViewKind::Surface) {
			REX::ERROR("ViewManifest: [content] {} fullscreen applies only to world views", Utf8Path(a_path));
			return std::nullopt;
		}
		manifest.capturesInput = Json::Get(*json, "capturesInput", manifest.capturesInput);
		manifest.pausesGame = Json::Get(*json, "pausesGame", manifest.pausesGame);
		manifest.openOnStart = Json::Get(*json, "openOnStart", manifest.openOnStart);
		manifest.order = static_cast<std::int32_t>(Json::Get(*json, "order", manifest.order));
		manifest.debugOnly = Json::Get(*json, "debugOnly", manifest.debugOnly);
		if (const auto launcher = json->find("launcher"); launcher != json->end() && *launcher != false) {
			if ((!launcher->is_object() && !launcher->is_boolean()) || (launcher->contains("modId") && !(*launcher)["modId"].is_string()) ||
				(launcher->contains("modTitle") && !(*launcher)["modTitle"].is_string()) || manifest.kind != ViewKind::Menu) {
				REX::ERROR("ViewManifest: {} launcher must be false, or opt in a menu with true or an object with optional string modId and modTitle", Utf8Path(a_path));
				return std::nullopt;
			}
			manifest.launcherMod = launcher->is_object() ? launcher->value("modId", manifest.mod) : manifest.mod;
			manifest.launcherModTitle = launcher->is_object() ? launcher->value("modTitle", manifest.title) : manifest.title;
			if (manifest.launcherMod.empty() || manifest.launcherMod.size() > 128 || manifest.launcherMod == "." || manifest.launcherMod == ".." ||
				manifest.launcherMod.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._-") != std::string::npos) {
				REX::ERROR("ViewManifest: {} launcher mod ID must be a valid OSF Settings mod ID; set launcher.modId if the mod folder name is incompatible", Utf8Path(a_path));
				return std::nullopt;
			}
		}

		const auto entryPath = std::filesystem::path(manifest.entry);
		if (entryPath.is_absolute() ||
			std::ranges::any_of(entryPath, [](const auto& part) { return part == ".."; })) {
			REX::ERROR("ViewManifest: [content] {} entry '{}' must be a relative path inside the view folder", Utf8Path(a_path), manifest.entry);
			return std::nullopt;
		}

		// HUDs and world views must never capture input or pause the game.
		if (manifest.kind != ViewKind::Menu) {
			if (manifest.capturesInput || manifest.pausesGame) {
				REX::WARN("ViewManifest: [content] Non-menu view '{}' cannot capture input or pause; forcing both off", manifest.id);
			}
			manifest.capturesInput = false;
			manifest.pausesGame = false;
		}

		return manifest;
	}
}
