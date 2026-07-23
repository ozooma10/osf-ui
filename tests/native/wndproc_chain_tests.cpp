#include "input/WndProcChain.h"

#include <cassert>
#include <iostream>

int main()
{
	using OSFUI::OverlayInputHook::detail::OriginalMovedAboveUs;

	constexpr std::uintptr_t game = 0x1000;
	constexpr std::uintptr_t originalHook = 0x2000;
	constexpr std::uintptr_t osfui = 0x3000;
	constexpr std::uintptr_t laterHook = 0x4000;

	assert(!OriginalMovedAboveUs(osfui, osfui, originalHook));
	assert(OriginalMovedAboveUs(originalHook, osfui, originalHook));
	assert(!OriginalMovedAboveUs(laterHook, osfui, originalHook));
	assert(!OriginalMovedAboveUs(osfui, osfui, game));

	std::cout << "WndProc chain tests passed\n";
}
