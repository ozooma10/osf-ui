#pragma once

namespace OSFUI
{
	// Menu-mode query over the engine's active menu array (mcm-design.md §9,
	// hotkey gameplay gate). kModal is the engine's own gameplay/menu
	// discriminator (RE-proven, see FocusMenu.cpp): gameplay-context menus
	// (HUDMenu, CursorMenu, FaderMenu, ...) have it clear; menus that take the
	// player out of gameplay (PauseMenu, ContainerMenu, DataMenu, dialogue,
	// MainMenu, ...) have it set. Our FocusMenu never sets it, so the OSF UI
	// overlay itself does not read as a game menu here.
	namespace MenuMode
	{
		// Game MAIN thread only (MainThreadMenuPump; SFSE-task ticks run on a
		// render-graph worker and must use the pump's snapshot instead): walks
		// RE::UI's active menu array.
		// True while any admitted kModal menu is open, i.e. not plain gameplay.
		// Also true while the dev console is open: it is kModal-clear (gameplay
		// keeps running) but console typing must not fire hotkeys.
		// A null UI singleton (boot) also reads as true: nothing is gameplay yet.
		[[nodiscard]] bool AnyGameMenuOpen();
	}
}
