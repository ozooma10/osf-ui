#include "core/Config.h"

#include <cassert>
#include <iostream>

int main()
{
	const auto config = OSFUI::Config::Load("../../data/OSFUI/config.json");

	// The shipped config deliberately omits backend selections. Its compiled
	// fallbacks must always describe a usable in-game production stack.
	assert(config.renderer == "webview2");
	assert(config.compositor == "d3d12");
	assert(config.inputSource == "ui");

	std::cout << "config defaults tests passed\n";
}
