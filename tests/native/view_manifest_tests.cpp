#include "Views/ViewManifest.h"

#include "Core/Log.h"
#include <cassert>
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
		assert(out.good());
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
	assert(manifest);
	assert(manifest->id == "demo.mod/terminal");
	assert(manifest->title == "Cargo terminal");

	Write(path, R"({
		"manifestVersion": 1,
		"id": "some-old-name"
	})");
	manifest = OSFUI::ViewManifest::Load(path);
	assert(manifest);
	assert(manifest->id == "demo.mod/terminal");

	Write(path, R"({
		"manifestVersion": 1,
		"entry": "index.html?mode=compact#inventory"
	})");
	manifest = OSFUI::ViewManifest::Load(path);
	assert(manifest);
	assert(manifest->entry == "index.html?mode=compact#inventory");

	Write(path, R"({ "kind": "hud" })");
	assert(!OSFUI::ViewManifest::Load(path));
	Write(path, R"({ "manifestVersion": 2, "kind": "hud" })");
	assert(!OSFUI::ViewManifest::Load(path));
	Write(path, R"({ "manifestVersion": 1, "kind": "future" })");
	assert(!OSFUI::ViewManifest::Load(path));

	std::filesystem::remove_all(root);
	std::cout << "view_manifest_tests: ok\n";
	return 0;
}
