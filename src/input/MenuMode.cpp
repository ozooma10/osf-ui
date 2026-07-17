#include "input/MenuMode.h"

#include "RE/I/IMenu.h"
#include "RE/U/UI.h"

namespace OSFUI
{
	bool MenuMode::AnyGameMenuOpen()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return true;
		}
		// UI+0x430 is the active (admitted) menu array — the same one the
		// engine's top-modal selector walks (FocusMenu.cpp Route A notes).
		// Only reading name-less flag bits here; no dynamic_cast, no vfuncs.
		for (const auto& menu : ui->menuArray) {
			if (menu && (menu->flags & RE::IMenu::kModal) != 0) {
				return true;
			}
		}
		return false;
	}
}
