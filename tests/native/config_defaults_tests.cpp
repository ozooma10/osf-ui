#include "core/Config.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

int main()
{
	const auto config = OSFUI::Config::Load("../../data/OSFUI/config.json");

	// The shipped config deliberately omits backend selections. Its compiled
	// fallbacks must always describe a usable in-game production stack.
	assert(config.renderer == "webview2");
	assert(config.compositor == "d3d12");
	assert(config.inputSource == "ui");

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

	std::cout << "config defaults tests passed\n";
}
