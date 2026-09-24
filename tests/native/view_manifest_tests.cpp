#include "Views/ViewManifest.h"
#include "Views/ViewManager.h"

#include "Core/Ids.h"
#include "Core/Json.h"
#include "Core/Log.h"
#include "Core/Utf8Path.h"
#include "stubs/check.h"
#include <fstream>
#include <iostream>

namespace
{
	std::filesystem::path MakeRoot()
	{
		const auto root = std::filesystem::temp_directory_path() /
			("osfui-view-manifest-" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directories(root / "demo.mod" / "terminal");
		return root;
	}

	void Write(const std::filesystem::path& a_path, std::string_view a_json)
	{
		std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
		out << a_json;
		CHECK(out.good());
	}
}

namespace OSFUI::Log
{
	void WarnOnce(std::once_flag& a_flag, std::string_view a_message)
	{
		std::call_once(a_flag, [&] { REX::test::Log("WARN", std::string(a_message)); });
	}
	bool DebugEnabled() { return true; }
	void SetDebugLogging(bool) {}
}

int main()
{
	const auto root = MakeRoot();
	const auto path = root / "demo.mod" / "terminal" / "manifest.json";

	// No "id" field: identity comes from the folder path alone.
	Write(path, R"({
		"manifestVersion": 1,
		"title": "Cargo terminal"
	})");
	auto manifest = OSFUI::ViewManifest::Load(path);
	CHECK(manifest);
	CHECK(manifest && manifest->id == "demo.mod/terminal");
	CHECK(manifest && manifest->title == "Cargo terminal");
	CHECK(manifest && manifest->launcherMod.empty());
	Write(path, R"({"manifestVersion":1,"launcher":{"modId":"demo.mod","modTitle":"Demo"}})");
	manifest = OSFUI::ViewManifest::Load(path);
	CHECK(manifest && manifest->launcherMod == "demo.mod" && manifest->launcherModTitle == "Demo");
	for (const auto invalid : {
		R"({"manifestVersion":1,"launcher":true})",
		R"({"manifestVersion":1,"launcher":{"modId":"Invalid ID"}})",
		R"({"manifestVersion":1,"kind":"hud","launcher":{"modId":"demo"}})",
		R"({"manifestVersion":1,"launcher":{"modId":"demo","modTitle":1}})"
	}) {
		Write(path, invalid);
		CHECK(!OSFUI::ViewManifest::Load(path));
	}

	Write(path, R"({
		"manifestVersion": 1,
		"id": "some-old-name"
	})");
	manifest = OSFUI::ViewManifest::Load(path);
	CHECK(manifest);
	CHECK(manifest && manifest->id == "demo.mod/terminal");

	Write(path, R"({
		"manifestVersion": 1,
		"entry": "index.html?mode=compact#inventory"
	})");
	manifest = OSFUI::ViewManifest::Load(path);
	CHECK(manifest);
	CHECK(manifest && manifest->entry == "index.html?mode=compact#inventory");

	Write(path, R"({ "kind": "hud" })");
	CHECK(!OSFUI::ViewManifest::Load(path));
	Write(path, R"({ "manifestVersion": 2, "kind": "hud" })");
	CHECK(!OSFUI::ViewManifest::Load(path));
	Write(path, R"({ "manifestVersion": 1, "kind": "future" })");
	CHECK(!OSFUI::ViewManifest::Load(path));

	// A Unicode install path is valid; identity folders must survive URL parsing
	// unchanged. Exercise the real discovery loop so one bad folder cannot abort it.
	const auto viewsRoot = root / u8"\u6a21\u7ec4" / "views";
	const auto addView = [&](const std::filesystem::path& a_mod, const std::filesystem::path& a_view) {
		const auto manifestPath = viewsRoot / a_mod / a_view / "manifest.json";
		std::filesystem::create_directories(manifestPath.parent_path());
		Write(manifestPath, R"({"manifestVersion":1})");
		return manifestPath;
	};
	const auto validPath = addView("Acme.widgets_2~demo", "main-menu");
	CHECK(OSFUI::ViewManifest::Load(validPath));
	for (const auto& invalidMod : {
		std::filesystem::path(u8"\u6a21\u7ec4"), std::filesystem::path("bad name"),
		std::filesystem::path("bad+name"), std::filesystem::path("bad;name"),
		std::filesystem::path("bad%20name") }) {
		const auto invalidPath = addView(invalidMod, "main");
		CHECK(!OSFUI::ViewManifest::Load(invalidPath));
	}
	for (const auto& invalidView : {
		std::filesystem::path(u8"\u89c6\u56fe"), std::filesystem::path("bad view"),
		std::filesystem::path("bad_view") }) {
		CHECK(!OSFUI::ViewManifest::Load(addView("valid-mod", invalidView)));
	}
	for (const auto invalidMod : { ".", "..", "trailing.", "CON", "nul.json", "COM1", "osfui" }) {
		CHECK(!OSFUI::Ids::IsValidModId(invalidMod));
	}
	CHECK(OSFUI::Ids::IsAcceptedModId("osfui"));
	CHECK(!OSFUI::Ids::IsValidQualifiedViewId("osfui/settings"));
	CHECK(!OSFUI::Ids::IsValidQualifiedViewId("osfui/keybinds"));

	const auto brokenPath = addView("broken", "main");
	Write(brokenPath, "{ broken JSON");
	// Both diagnostics paths must format Unicode filenames without throwing,
	// including the parse-error catch block.
	CHECK(!OSFUI::Json::ParseFile(brokenPath));
	CHECK(!OSFUI::Json::ParseFile(viewsRoot / u8"\u4e0d\u5b58\u5728.json"));
	CHECK(REX::test::Entries().back().find(OSFUI::Utf8Path(viewsRoot)) != std::string::npos);

	OSFUI::ViewManager views;
	views.DiscoverAll(viewsRoot);
	CHECK(views.All().size() == 1);
	CHECK(views.Find("Acme.widgets_2~demo/main-menu"));
	views.DiscoverAll(root / u8"\u4e0d\u5b58\u5728");
	CHECK(views.All().empty());

	std::filesystem::remove_all(root);
	std::cout << "view_manifest_tests: " << g_checks << " checks, " << g_failures << " failures\n";
	return g_failures;
}
