#include <cassert>
#include "Input/KeyOwnership.h"
#include "Input/OverlayInputHook.h"

#include <cassert>
#include <iostream>

int main()
{
	OSFUI::KeyOwnership keys;
	assert(!keys.Consume(0x57, true, false));  // game receives W down
	assert(!keys.Consume(0x57, false, true));  // game still receives W up during capture
	assert(keys.Consume(0x1B, true, true));    // overlay closes on Escape down
	assert(keys.Consume(0x1B, true, false));   // held repeat stays with overlay after close
	assert(keys.Consume(0x1B, false, false));  // release cannot leak into PauseMenu
	assert(!keys.Consume(0x1B, true, false));  // next complete press belongs to game
	keys.Reset();

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
