#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "Input/RawInputPacket.h"
#include "Input/KeyOwnership.h"
#include <cassert>
#include <iostream>

int main()
{
	RAWINPUT keyboard{};
	keyboard.header.dwType = RIM_TYPEKEYBOARD;
	keyboard.header.dwSize = offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD);
	keyboard.data.keyboard.VKey = VK_F7;
	const auto bytes = keyboard.header.dwSize;
	assert(bytes < sizeof(RAWINPUT)); // The old capacity comparison rejected this packet.
	assert(OSFUI::IsCompleteRawKeyboard(keyboard, bytes));
	assert(!OSFUI::IsCompleteRawKeyboard(keyboard, bytes - 1));
	assert(!OSFUI::IsCompleteRawKeyboard(keyboard, UINT(-1)));
	assert(!OSFUI::IsCompleteRawKeyboard(keyboard, sizeof(RAWINPUT) + 1));
	keyboard.header.dwType = RIM_TYPEMOUSE;
	assert(!OSFUI::IsCompleteRawKeyboard(keyboard, bytes));
	keyboard.header.dwType = RIM_TYPEKEYBOARD;
	keyboard.header.dwSize = bytes + 1;
	assert(!OSFUI::IsCompleteRawKeyboard(keyboard, bytes));
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
		assert(!consume(false));
		keyboard.data.keyboard.Flags = RI_KEY_BREAK;
		assert(!consume(true));
	}
	std::cout << "Raw keyboard packet and capture-transition tests passed (packet="
		<< bytes << ", capacity=" << sizeof(RAWINPUT) << ")\n";
}
