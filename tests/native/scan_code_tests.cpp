// Native desktop tests for the physical key identity core: ComposeScanCode's
// message-quirk normalization (Input/ScanCode.h), the kNamedScans name table
// (Input/KeyNames.cpp — full-table round-trip and the ≤16-char name
// constraint), the W3C KeyboardEvent.code alias
// vocabulary, and the frozen legacy VK resolver the values migration depends
// on. Assert-style; process exit code is the failure count.

#include "Input/KeyNames.h"
#include "Input/ScanCode.h"
#include "check.h"

namespace
{

}

// KeyNames.cpp references the Log seam; provide the standard test stub
// (same shape as the other suites).
namespace OSFUI::Log
{
	void WarnOnce(std::once_flag& a_flag, std::string_view a_message)
	{
		std::call_once(a_flag, [&] { REX::test::Log("WARN", std::string(a_message)); });
	}

	bool DebugEnabled() { return false; }
	void SetDebugLogging(bool) {}
}

int main()
{
	using OSFUI::ComposeScanCode;
	using OSFUI::KeyName;
	using OSFUI::ResolveKeyName;
	using OSFUI::ScanCode;

	// ---- ComposeScanCode: plain, extended, and the three quirk keys --------
	{
		// W: raw make code passes through untouched.
		CHECK(ComposeScanCode(0x57 /*VK W*/, 0x11, false) == 0x11);
		// Up arrow: E0-prefixed, folds to the DIK 0x80 convention.
		CHECK(ComposeScanCode(0x26 /*VK_UP*/, 0x48, true) == 0xC8);
		// RCtrl vs LCtrl differ only by the extended bit.
		CHECK(ComposeScanCode(0xA2 /*VK_LCONTROL*/, 0x1D, false) == 0x1D);
		CHECK(ComposeScanCode(0xA3 /*VK_RCONTROL*/, 0x1D, true) == 0x9D);
		// Pause reports raw 0x45 with the extended bit CLEAR — colliding with
		// NumLock's fields; only the VK disambiguates. DIK_PAUSE = 0xC5.
		CHECK(ComposeScanCode(0x13 /*VK_PAUSE*/, 0x45, false) == 0xC5);
		// NumLock pins to DIK_NUMLOCK 0x45 even when a path sets the ext bit.
		CHECK(ComposeScanCode(0x90 /*VK_NUMLOCK*/, 0x45, true) == 0x45);
		// PrintScreen pins to DIK_SYSRQ 0xB7 regardless of message fields —
		// including a zero raw scan.
		CHECK(ComposeScanCode(0x2C /*VK_SNAPSHOT*/, 0x37, true) == 0xB7);
		CHECK(ComposeScanCode(0x2C /*VK_SNAPSHOT*/, 0x00, false) == 0xB7);
		// No scan fields at all (SendInput-synthesized): invalid, caller falls
		// back to the platform VK->scan mapping.
		CHECK(ComposeScanCode(0x41 /*VK A*/, 0x00, false) == OSFUI::kInvalidScanCode);
	}

	// ---- Full-table sweep: every nameable code round-trips, names ≤16 ------
	// KeyName returns the FIRST kNamedScans row per code (the canonical
	// spelling); ResolveKeyName must turn that exact string back into the same
	// code — the property every saved binding depends on. The 16-char bound is
	// the persisted key-value constraint.
	{
		int named = 0;
		for (unsigned code = 1; code <= 0xFF; ++code) {
			const auto name = KeyName(static_cast<ScanCode>(code));
			if (name.empty()) {
				continue;
			}
			++named;
			CHECK(ResolveKeyName(name) == code);
			CHECK(name.size() <= 16);
		}
		// Letters + digits + F1-F24 + punctuation + nav + numpad + the rest:
		// the table is substantial. Guard against accidental mass deletion.
		CHECK(named >= 100);
	}

	// ---- Physical anchors (DIK values, same space as the controlmap) -------
	{
		CHECK(ResolveKeyName("Escape") == 0x01);
		CHECK(ResolveKeyName("W") == 0x11);
		CHECK(ResolveKeyName("Semicolon") == 0x27);
		CHECK(ResolveKeyName("Apostrophe") == 0x28);
		CHECK(ResolveKeyName("Grave") == 0x29);
		CHECK(ResolveKeyName("Space") == 0x39);
		CHECK(ResolveKeyName("F5") == 0x3F);
		CHECK(ResolveKeyName("F12") == 0x58);
		CHECK(ResolveKeyName("F24") == 0x76);
		CHECK(ResolveKeyName("Up") == 0xC8);
		CHECK(ResolveKeyName("LShift") == 0x2A);
		CHECK(ResolveKeyName("RShift") == 0x36);
		CHECK(ResolveKeyName("Enter") == 0x1C);
		// Previously unbindable keys now have identities of their own.
		CHECK(ResolveKeyName("IntlBackslash") == 0x56);
		CHECK(ResolveKeyName("NumpadEnter") == 0x9C);
		CHECK(ResolveKeyName("Numpad0") == 0x52);
		CHECK(ResolveKeyName("NumpadDivide") == 0xB5);
		CHECK(ResolveKeyName("PrintScreen") == 0xB7);
		CHECK(ResolveKeyName("Apps") == 0xDD);
		CHECK(ResolveKeyName("LWin") == 0xDB);
		// Numpad keys are distinct from their main-block twins.
		CHECK(ResolveKeyName("NumpadEnter") != ResolveKeyName("Enter"));
		CHECK(ResolveKeyName("Numpad1") != ResolveKeyName("1"));
		CHECK(ResolveKeyName("NumpadDecimal") != ResolveKeyName("Period"));
	}

	// ---- W3C KeyboardEvent.code spellings fold to the canonical names ------
	{
		CHECK(ResolveKeyName("BracketLeft") == ResolveKeyName("LBracket"));
		CHECK(ResolveKeyName("BracketRight") == ResolveKeyName("RBracket"));
		CHECK(ResolveKeyName("Backquote") == ResolveKeyName("Grave"));
		CHECK(ResolveKeyName("ShiftLeft") == ResolveKeyName("LShift"));
		CHECK(ResolveKeyName("ControlRight") == ResolveKeyName("RCtrl"));
		CHECK(ResolveKeyName("AltRight") == ResolveKeyName("RAlt"));
		CHECK(ResolveKeyName("ArrowUp") == ResolveKeyName("Up"));
		CHECK(ResolveKeyName("ArrowLeft") == ResolveKeyName("Left"));
		CHECK(ResolveKeyName("ContextMenu") == ResolveKeyName("Apps"));
		CHECK(ResolveKeyName("MetaLeft") == ResolveKeyName("LWin"));
		CHECK(ResolveKeyName("Oem102") == ResolveKeyName("IntlBackslash"));
		// Aliases fold to the canonical spelling on the way back out.
		CHECK(KeyName(ResolveKeyName("Backquote")) == "Grave");
		CHECK(KeyName(ResolveKeyName("ShiftLeft")) == "LShift");
		CHECK(KeyName(ResolveKeyName("Return")) == "Enter");
	}

	// ---- Case-insensitive resolution, unknown names ------------------------
	{
		CHECK(ResolveKeyName("w") == 0x11);
		CHECK(KeyName(ResolveKeyName("w")) == "W");
		CHECK(ResolveKeyName("semicolon") == 0x27);
		CHECK(ResolveKeyName("f10") == 0x44);
		CHECK(ResolveKeyName("") == OSFUI::kInvalidScanCode);
		CHECK(ResolveKeyName("NotAKey") == OSFUI::kInvalidScanCode);
	}

	// ---- Frozen legacy VK resolver (migration input) ------------------------
	// Pins the pre-2.x semantics the one-time values migration interprets:
	// VK-anchored, arithmetic F-keys and alnum, US ANSI OEM meanings. Never
	// extended — new names deliberately do NOT resolve here.
	{
		using OSFUI::Legacy::ResolveKeyNameVk;
		CHECK(ResolveKeyNameVk("Semicolon") == 0xBA);
		CHECK(ResolveKeyNameVk("Grave") == 0xC0);
		CHECK(ResolveKeyNameVk("Tilde") == 0xC0);
		CHECK(ResolveKeyNameVk("LBracket") == 0xDB);
		CHECK(ResolveKeyNameVk("F10") == 0x79);
		CHECK(ResolveKeyNameVk("w") == 0x57);
		CHECK(ResolveKeyNameVk("7") == 0x37);
		CHECK(ResolveKeyNameVk("Space") == 0x20);
		CHECK(ResolveKeyNameVk("LShift") == 0xA0);
		CHECK(ResolveKeyNameVk("") == 0);
		CHECK(ResolveKeyNameVk("IntlBackslash") == 0);
		CHECK(ResolveKeyNameVk("NumpadEnter") == 0);
	}

	std::fprintf(stderr, "scan_code_tests: %d checks, %d failure(s)\n", g_checks, g_failures);
	return g_failures;
}
