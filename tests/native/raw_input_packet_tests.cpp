#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "Input/RawInputPacket.h"
#include "check.h"
#include <iostream>

int main()
{
	RAWINPUT keyboard{};
	keyboard.header.dwType = RIM_TYPEKEYBOARD;
	keyboard.header.dwSize = offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD);
	keyboard.data.keyboard.VKey = VK_F7;
	const auto bytes = keyboard.header.dwSize;
	CHECK(bytes < sizeof(RAWINPUT)); // The old capacity comparison rejected this packet.
	CHECK(OSFUI::IsCompleteRawKeyboard(keyboard, bytes));
	CHECK(!OSFUI::IsCompleteRawKeyboard(keyboard, bytes - 1));
	CHECK(!OSFUI::IsCompleteRawKeyboard(keyboard, UINT(-1)));
	CHECK(!OSFUI::IsCompleteRawKeyboard(keyboard, sizeof(RAWINPUT) + 1));
	keyboard.header.dwType = RIM_TYPEMOUSE;
	CHECK(!OSFUI::IsCompleteRawKeyboard(keyboard, bytes));
	keyboard.header.dwType = RIM_TYPEKEYBOARD;
	keyboard.header.dwSize = bytes + 1;
	CHECK(!OSFUI::IsCompleteRawKeyboard(keyboard, bytes));
	keyboard.header.dwSize = bytes;

	std::cout << "Raw keyboard packet tests: " << g_checks
		<< " checks, " << g_failures << " failures (packet=" << bytes
		<< ", capacity=" << sizeof(RAWINPUT) << ")\n";
	return g_failures ? 1 : 0;
}
