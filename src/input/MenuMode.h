#pragma once

namespace OSFUI
{
	// Menu-mode query over the engine's active menu array (mcm-design.md §9,
	// hotkey gameplay gate). The kModal flag is the engine's own gameplay/menu
	// discriminator (RE-proven, see FocusMenu.cpp): every gameplay-context menu
	// (HUDMenu, CursorMenu, FaderMenu, ...) has it CLEAR; every menu that takes
	// the player out of gameplay (PauseMenu, ContainerMenu, DataMenu, dialogue,
	// MainMenu, ...) has it SET. Our own FocusMenu deliberately never sets it,
	// so the OSF UI overlay itself never reads as a game menu here.
	namespace MenuMode
	{
		// GAME thread only (Runtime::Tick): walks RE::UI's active menu array.
		// True while any admitted kModal menu is open — i.e. NOT plain gameplay.
		// A null UI singleton (boot) also reads as true: nothing is gameplay yet.
		[[nodiscard]] bool AnyGameMenuOpen();
	}
}
