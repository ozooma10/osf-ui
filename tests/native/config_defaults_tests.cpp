#include "core/Config.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>

namespace
{
	bool LoggedContaining(std::string_view a_level, std::string_view a_needle)
	{
		return std::ranges::any_of(REX::test::Entries(), [&](const std::string& e) {
			return e.starts_with(a_level) && e.find(a_needle) != std::string::npos;
		});
	}
}

int main()
{
	const auto config = OSFUI::Config::Load("../../data/OSFUI/config.json");

	// The shipped config deliberately omits backend selections. Its compiled
	// fallbacks must always describe a usable in-game production stack.
	assert(config.inputSource == "ui");
	assert(config.view == "osfui/settings");
	assert(!LoggedContaining("WARN", "unknown key"));

	// captureInput remains a boot-file gate, while pauseMenuEntry is owned by
	// the live MCM store and must ignore a stale config.json override.
	const std::filesystem::path overridePath = ".build/config-overrides.json";
	std::filesystem::create_directories(overridePath.parent_path());
	{
		std::ofstream out(overridePath);
		out << R"({"captureInput":false,"pauseMenuEntry":false})";
	}
	const auto overrides = OSFUI::Config::Load(overridePath);
	assert(!overrides.captureInput);
	assert(overrides.pauseMenuEntry);
	std::filesystem::remove(overridePath);

	// There is no config-v1 compatibility branch. Removed fields are ordinary
	// unknown keys, while retained fields still parse and unknown values remain
	// harmless.
	{
		std::ofstream out(overridePath);
		out << R"({"configVersion":1,"view":"osfui/settings","views":["osfui/settings","a.b/hud"],"warmViews":["osfui/settings"],"devMode":true})";
	}
	const auto legacy = OSFUI::Config::Load(overridePath);
	assert(legacy.devMode);
	assert(legacy.view == "osfui/settings");
	assert(LoggedContaining("WARN", "unknown key 'views'"));
	assert(LoggedContaining("WARN", "unknown key 'warmViews'"));
	std::filesystem::remove(overridePath);

	std::cout << "config defaults tests passed\n";
}
