#include "Input/ScanCode.h"
#include "check.h"

int main()
{
	using OSFUI::ComposeScanCode;

	// ---- ComposeScanCode: plain, extended, and the three quirk keys --------
	{
		// W: raw make code passes through untouched.
		CHECK(ComposeScanCode(0x57 /*VK W*/, 0x11, false) == 0x11);
		// Up arrow: E0-prefixed, folds to the DIK 0x80 convention.
		CHECK(ComposeScanCode(0x26 /*VK_UP*/, 0x48, true) == 0xC8);
		// RCtrl vs LCtrl differ only by the extended bit.
		CHECK(ComposeScanCode(0xA2 /*VK_LCONTROL*/, 0x1D, false) == 0x1D);
		CHECK(ComposeScanCode(0xA3 /*VK_RCONTROL*/, 0x1D, true) == 0x9D);
		CHECK(ComposeScanCode(0x13 /*VK_PAUSE*/, 0x45, false) == 0xC5);
		// NumLock pins to DIK_NUMLOCK 0x45 even when a path sets the ext bit.
		CHECK(ComposeScanCode(0x90 /*VK_NUMLOCK*/, 0x45, true) == 0x45);
		CHECK(ComposeScanCode(0x2C /*VK_SNAPSHOT*/, 0x37, true) == 0xB7);
		CHECK(ComposeScanCode(0x2C /*VK_SNAPSHOT*/, 0x00, false) == 0xB7);
		CHECK(ComposeScanCode(0x41 /*VK A*/, 0x00, false) == OSFUI::kInvalidScanCode);
	}

	std::fprintf(stderr, "scan_code_tests: %d checks, %d failure(s)\n", g_checks, g_failures);
	return g_failures;
}
