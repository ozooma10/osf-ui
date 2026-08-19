#pragma once

namespace OSFUI
{
	// Treat admitted kModal menus as non-gameplay; FocusMenu deliberately remains kModal-clear.
	namespace MenuMode
	{
		// Main-thread only; fail closed at boot and treat the kModal-clear console as non-gameplay.
		[[nodiscard]] bool AnyGameMenuOpen();
	}
}
