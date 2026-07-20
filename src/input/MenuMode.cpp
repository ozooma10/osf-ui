#include "input/MenuMode.h"

#include "RE/I/IMenu.h"
#include "RE/U/UI.h"

#include "input/MenuEventSink.h"

namespace OSFUI
{
	bool MenuMode::AnyGameMenuOpen()
	{
		// The dev console is a gameplay-context menu (kModal clear — the world keeps
		// running behind it), so the flag walk below misses it, yet every keystroke
		// typed into it is a key-down edge that would fire mods' bindings. Two
		// independent detectors, either blocks: the open/close edge tracked by
		// MenuEventSink, and a name check in the admitted-array walk.
		// RE::UI::IsMenuOpen("Console") returned false with the console open
		// (2026-07-18 live run), so the engine helper is not trusted here.
		if (MenuEventSink::ConsoleOpen()) {
			return true;
		}
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return true;
		}
		// UI+0x430 is the active (admitted) menu array — the same one the engine's
		// top-modal selector walks (see FocusMenu.cpp, Route A). Flag bits + the
		// interned +0xB0 name only; no dynamic_cast, no vfuncs.
		for (const auto& menu : ui->menuArray) {
			if (!menu) {
				continue;
			}
			if ((menu->flags & RE::IMenu::kModal) != 0) {
				return true;
			}
			if (menu->menuName == std::string_view{ "Console" }) {
				return true;
			}
		}
		return false;
	}
}
