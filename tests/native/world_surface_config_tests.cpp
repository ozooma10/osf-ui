#include "core/Config.h"

#include <cassert>
#include <fstream>
#include <iostream>

// The worldSurfaces array is the safety gate between config typos and a
// hooked engine device: entries that could ever collide with an engine
// render target, another surface, or bridge routing must be dropped, and
// dropping must never take the rest of the config with it.

namespace
{
	std::filesystem::path WriteConfig(std::string_view a_json)
	{
		static int counter = 0;
		const auto path =
			std::filesystem::temp_directory_path() /
			("osfui-world-surface-config-" +
				std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
				"-" + std::to_string(counter++) + ".json");
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << a_json;
		assert(out.good());
		return path;
	}

	OSFUI::Config Load(std::string_view a_json)
	{
		const auto path = WriteConfig(a_json);
		auto config = OSFUI::Config::Load(path);
		std::filesystem::remove(path);
		return config;
	}
}  // namespace

int main()
{
	using OSFUI::Config;

	// The size rules stand alone: square, 256-8192, never a power of two.
	assert(Config::CheckPlaceholderSize(1000, 1000).empty());
	assert(Config::CheckPlaceholderSize(998, 998).empty());
	assert(!Config::CheckPlaceholderSize(1600, 900).empty());   // screen-shaped
	assert(!Config::CheckPlaceholderSize(1024, 1024).empty());  // power of two
	assert(!Config::CheckPlaceholderSize(1000, 998).empty());   // non-square
	assert(!Config::CheckPlaceholderSize(100, 100).empty());    // too small
	assert(!Config::CheckPlaceholderSize(8200, 8200).empty());  // too large
	assert(!Config::CheckPlaceholderSize(0, 0).empty());        // unset

	// Absent key: no surfaces, and the rest of the config still parses.
	{
		const auto config = Load(R"({ "devMode": true })");
		assert(config.worldSurfaces.empty());
		assert(config.devMode);
	}

	// One valid entry, with browser-size clamping applied.
	{
		const auto config = Load(R"({ "worldSurfaces": [
			{ "view": "mymod.screens/terminal", "width": 9000, "height": 32,
			  "placeholderWidth": 1000, "placeholderHeight": 1000 } ] })");
		assert(config.worldSurfaces.size() == 1);
		assert(config.worldSurfaces[0].view == "mymod.screens/terminal");
		assert(config.worldSurfaces[0].width == 4096);
		assert(config.worldSurfaces[0].height == 64);
		assert(config.worldSurfaces[0].placeholderWidth == 1000);
		assert(config.worldSurfaces[0].placeholderHeight == 1000);
	}

	// Defaults: width/height fall back to 1600x900 when omitted.
	{
		const auto config = Load(R"({ "worldSurfaces": [
			{ "view": "a.b/v", "placeholderWidth": 998, "placeholderHeight": 998 } ] })");
		assert(config.worldSurfaces.size() == 1);
		assert(config.worldSurfaces[0].width == 1600);
		assert(config.worldSurfaces[0].height == 900);
	}

	// Two entries with unique sizes and views survive in file order.
	{
		const auto config = Load(R"({ "worldSurfaces": [
			{ "view": "a.b/one", "placeholderWidth": 1000, "placeholderHeight": 1000 },
			{ "view": "a.b/two", "placeholderWidth": 998, "placeholderHeight": 998 } ] })");
		assert(config.worldSurfaces.size() == 2);
		assert(config.worldSurfaces[0].view == "a.b/one");
		assert(config.worldSurfaces[1].view == "a.b/two");
	}

	// The SRV hook keys on size alone: a repeated placeholder size is dropped.
	{
		const auto config = Load(R"({ "worldSurfaces": [
			{ "view": "a.b/one", "placeholderWidth": 1000, "placeholderHeight": 1000 },
			{ "view": "a.b/two", "placeholderWidth": 1000, "placeholderHeight": 1000 } ] })");
		assert(config.worldSurfaces.size() == 1);
		assert(config.worldSurfaces[0].view == "a.b/one");
	}

	// Bridge routing keys on view id: a repeated view is dropped.
	{
		const auto config = Load(R"({ "worldSurfaces": [
			{ "view": "a.b/one", "placeholderWidth": 1000, "placeholderHeight": 1000 },
			{ "view": "a.b/one", "placeholderWidth": 998, "placeholderHeight": 998 } ] })");
		assert(config.worldSurfaces.size() == 1);
		assert(config.worldSurfaces[0].placeholderWidth == 1000);
	}

	// The per-surface host-process cost model caps the list.
	{
		const auto config = Load(R"({ "worldSurfaces": [
			{ "view": "a.b/v1", "placeholderWidth": 990, "placeholderHeight": 990 },
			{ "view": "a.b/v2", "placeholderWidth": 992, "placeholderHeight": 992 },
			{ "view": "a.b/v3", "placeholderWidth": 994, "placeholderHeight": 994 },
			{ "view": "a.b/v4", "placeholderWidth": 996, "placeholderHeight": 996 },
			{ "view": "a.b/v5", "placeholderWidth": 998, "placeholderHeight": 998 } ] })");
		assert(config.worldSurfaces.size() == Config::kMaxWorldSurfaces);
		assert(config.worldSurfaces.back().view == "a.b/v4");
	}

	// Malformed entries drop individually; valid neighbors survive.
	{
		const auto config = Load(R"({ "worldSurfaces": [
			{ "placeholderWidth": 1000, "placeholderHeight": 1000 },
			"not-an-object",
			{ "view": "a.b/bad", "placeholderWidth": 1024, "placeholderHeight": 1024 },
			{ "view": "a.b/good", "placeholderWidth": 998, "placeholderHeight": 998 } ] })");
		assert(config.worldSurfaces.size() == 1);
		assert(config.worldSurfaces[0].view == "a.b/good");
	}

	// A stale research-era config still parses; its retired keys are ignored.
	{
		const auto config = Load(R"({ "worldSurfaceView": "a.b/old",
			"worldSurfaceTargetWidth": 1000, "devMode": true })");
		assert(config.worldSurfaces.empty());
		assert(config.devMode);
	}

	std::cout << "world surface config tests passed\n";
}
