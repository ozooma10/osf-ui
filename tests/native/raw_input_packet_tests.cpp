#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "Input/RawInputPacket.h"
#include "Input/KeyOwnership.h"
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

	// F7 opens the overlay on its down edge. Its shorter raw release must still
	// reach Starfield, or the next press is reported as a held repeat.
	OSFUI::KeyOwnership ownership;
	const auto consume = [&](bool captured) {
		return OSFUI::IsCompleteRawKeyboard(keyboard, bytes) ?
			ownership.Consume(keyboard.data.keyboard.VKey,
				(keyboard.data.keyboard.Flags & RI_KEY_BREAK) == 0, captured) : captured;
	};
	for (int cycle = 0; cycle < 8; ++cycle) {
		keyboard.data.keyboard.Flags = RI_KEY_MAKE;
		CHECK(!consume(false));
		keyboard.data.keyboard.Flags = RI_KEY_BREAK;
		CHECK(!consume(true));
	}
	std::cout << "Raw keyboard packet and capture-transition tests: " << g_checks
		<< " checks, " << g_failures << " failures (packet=" << bytes
		<< ", capacity=" << sizeof(RAWINPUT) << ")\n";
	return g_failures ? 1 : 0;
}
