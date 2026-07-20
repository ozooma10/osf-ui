#pragma once

// Runtime layout guard for the RE::UI singleton. Not a hook: it only reads the
// live object's vptr and compares it against the Address Library.
//
// Exists because of the 2026-06-12 incident (docs/reverse-engineering-notes.md):
// a CommonLibSF pinned before upstream PR #26 had UI base offsets short by 0x10;
// RegisterSink then corrupted engine UI state and the process died on save load
// with no plugin frames on the stack. The guard turns that into a loud refusal.
//
// Constant it checks: the BSInputEventReceiver vtable is RE::UI::VTABLE[10]
// (AddressLib ID 475439). IDs_VTABLE is not in base-declaration order — proven
// via tools/parse_versionlib.py plus the live vptr on 1.16.244. On mismatch the
// guard dumps every VTABLE entry so the index can be re-derived after a patch.

namespace OSFUI
{
	class UiLayoutGuard
	{
	public:
		// Proves the compiled UI layout matches the running binary (live vptr
		// vs AddressLib vtable). Must pass before anything touches the UI
		// object — including MenuEventSink's RegisterSink and FocusMenu's
		// RegisterMenu, which silently corrupt UI state on stale base offsets.
		// Logs on failure. Call once the UI singleton exists (kPostPostDataLoad).
		static bool VerifyUiLayout();
	};
}
