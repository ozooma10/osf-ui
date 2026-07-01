#pragma once

// Runtime layout guard for the RE::UI singleton. Not a hook: it only READS the
// live object's vptr and compares it against the Address Library.
//
// Why this exists (2026-06-12 incident, docs/reverse-engineering-notes.md):
// building against a CommonLibSF pinned before upstream PR #26 shipped a UI
// layout whose base offsets were short by 0x10; RegisterSink then corrupted
// engine UI state and the process died on save load with no plugin frames on
// the stack. The guard turns that class of failure into a loud refusal.
//
// Provenance of the constant it checks: the BSInputEventReceiver vtable is
// RE::UI::VTABLE[10] (AddressLib ID 475439). The IDs_VTABLE array is NOT in
// base-declaration order — proven via tools/parse_versionlib.py + the live
// vptr on 1.16.244. On mismatch the guard dumps every VTABLE entry so the
// index can be re-derived after a game patch.
//
// (Until 2026-07-01 this file also carried an observe-only vfunc hook on
// UI::PerformInputProcessing. It was diagnostic-only — input routing and
// consumption live at the WndProc subclass, input/OverlayInputHook — so the
// hook was removed; the guard is the part that must stay.)

namespace OSFUI
{
	class UiLayoutGuard
	{
	public:
		// Proves the compiled UI layout matches the running binary (live vptr
		// vs AddressLib vtable). Must pass before ANYTHING touches the UI
		// object — including MenuEventSink's RegisterSink and FocusMenu's
		// RegisterMenu, which silently corrupt UI state if the base offsets
		// are stale. Logs on failure. Call once the UI singleton exists
		// (kPostPostDataLoad).
		static bool VerifyUiLayout();
	};
}
