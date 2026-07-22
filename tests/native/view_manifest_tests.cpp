#include "runtime/ViewManifest.h"

#include "core/Log.h"
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
	bool DevMode() { return true; }
	void SetDevMode(bool) {}
}

int main()
{
	const auto root = MakeRoot();
	const auto path = root / "demo.mod" / "terminal" / "manifest.json";

	Write(path, R"({
		"id": "terminal",
		"title": "Cargo terminal",
		"accent": "#E6904A",
		"readySignal": true,
		"permissions": { "nativeBridge": true }
	})");
	auto manifest = OSFUI::ViewManifest::Load(path);
	assert(manifest);
	assert(manifest->id == "demo.mod/terminal");
	assert(manifest->accent == "#e6904a");
	assert(manifest->readySignal);

	// Explicit readiness cannot work without a bridge. The parser degrades to
	// load completion, so a typo cannot leave the handoff waiting forever.
	Write(path, R"({
		"id": "terminal",
		"accent": "#nothex",
		"readySignal": true,
		"permissions": { "nativeBridge": false }
	})");
	manifest = OSFUI::ViewManifest::Load(path);
	assert(manifest);
	assert(manifest->accent.empty());
	assert(!manifest->readySignal);

	std::filesystem::remove_all(root);
	std::cout << "view_manifest_tests: ok\n";
	return 0;
}
