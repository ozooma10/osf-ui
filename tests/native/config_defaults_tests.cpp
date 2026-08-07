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

	// The shipped config contains only the supported production inputs.
	assert(config.view == "osfui/settings");
	assert(!LoggedContaining("WARN", "unknown key"));

	// Removed input switches are ordinary unknown keys. The production WndProc,
	// capture, hardware-cursor, FocusMenu and engine-input path is not
	// configurable; pauseMenuEntry is likewise owned by the live Mod Settings state.
	const std::filesystem::path overridePath = ".build/config-overrides.json";
	std::filesystem::create_directories(overridePath.parent_path());
	{
		std::ofstream out(overridePath);
		out << R"({"inputSource":"none","captureInput":false,"hardwareCursor":false,"focusMenu":false,"engineInput":false,"pauseMenuEntry":false})";
	}
	const auto overrides = OSFUI::Config::Load(overridePath);
	assert(overrides.pauseMenuEntry);
	assert(LoggedContaining("WARN", "unknown key 'inputSource'"));
	assert(LoggedContaining("WARN", "unknown key 'captureInput'"));
	assert(LoggedContaining("WARN", "unknown key 'hardwareCursor'"));
	assert(LoggedContaining("WARN", "unknown key 'focusMenu'"));
	assert(LoggedContaining("WARN", "unknown key 'engineInput'"));
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
