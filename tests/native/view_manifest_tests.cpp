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

	// World feeds require an owned, generated asset path and opaque presentation.
    const std::string texture = "textures/osfui/feeds/0123456789abcdef0123456789abcdef/0123456789abcdef0123456789abcdef.dds";
    const auto world = std::string(R"({"manifestVersion":1,"kind":"world","texture":")") + texture + R"(")";
    Write(path, world + R"(,"width":1600,"height":900,"transparent":true,"capturesInput":true,"pausesGame":true,"openOnStart":true,"order":12})");
    manifest = OSFUI::ViewManifest::Load(path);
    assert(manifest && manifest->kind == OSFUI::ViewKind::World);
    assert(manifest->texture == texture && manifest->width == 1600 && manifest->height == 900);
    assert(!manifest->transparent && !manifest->menuInputEligible && !manifest->capturesInput);
    assert(!manifest->pausesGame && !manifest->openOnStart && manifest->order == 0);
    Write(path, R"({"manifestVersion":1,"kind":"world"})");
    assert(!OSFUI::ViewManifest::Load(path));
    Write(path, world + R"(,"placeholderSize":1000})");
    assert(!OSFUI::ViewManifest::Load(path));
    for (const auto key : {"width","height"}) {
        for (const auto size : {"null","false","1.5","-1","0","4097","4294968196","18446744073709551615"}) {
            Write(path, world + ",\"" + key + "\":" + size + "}");
            assert(!OSFUI::ViewManifest::Load(path));
        }
    }
    Write(path, world + R"(,"width":1,"height":4096})");
    manifest = OSFUI::ViewManifest::Load(path);
    assert(manifest && manifest->width == 1 && manifest->height == 4096);
    for (const auto bad : {"textures/vanilla.dds","../foreign.dds","textures/osfui/feeds/other.dds"}) {
        Write(path, std::string(R"({"manifestVersion":1,"kind":"world","texture":")") + bad + R"("})");
        assert(!OSFUI::ViewManifest::Load(path));
    }

	std::filesystem::remove_all(root);
	std::cout << "view_manifest_tests: ok\n";
	return 0;
}
