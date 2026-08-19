#include "Input/MenuMode.h"

#include "RE/I/IMenu.h"
#include "RE/U/UI.h"

#include "Input/MenuEventSink.h"

namespace OSFUI
{
	bool MenuMode::AnyGameMenuOpen()
	{
		// The console is kModal-clear, so block from its tracked edge or admitted name instead of IsMenuOpen.
		if (MenuEventSink::ConsoleOpen()) {
			return true;
		}
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return true;
		}
		// Walk UI+0x430 using flags and the interned +0xB0 name only.
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
