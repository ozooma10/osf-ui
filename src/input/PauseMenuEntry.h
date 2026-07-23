#pragma once

#include <string>

namespace OSFUI
{
	// Config `pauseMenuEntry` (on by default). Injects a "MOD MENUS" entry into
	// PauseMenu and opens the configured overlay view when pressed.
	//
	// This is live Scaleform manipulation rather than a SWF replacement, so it
	// works with both normal and large-font PauseMenu movies. The implementation
	// relies on three invariants verified against Starfield 1.16.244:
	//
	//   * Only the admitted PauseMenu with kAdvancesMovie and a live AS3 root is
	//     callable. The registration-map slot and MenuOpenCloseEvent are not
	//     lifecycle authorities; both lag teardown.
	//   * ALL engine-UI access (GetMenu, menuArray, AS3 invokes) must run on the
	//     game main thread, and AS3 calls must also avoid VM re-entry.
	//     Reconcile is therefore driven by MainThreadMenuPump after
	//     UI_AdvanceActiveMenus, where the engine owns the VM and it is idle.
	//   * Action 100 is consumed in the callback's originating movie before the
	//     current live movie is checked, so stale callbacks cannot leak the
	//     private action into the engine or open a replacement menu's overlay.
	//
	// The count gate keeps steady state to one entryCount read per pass. A native
	// list re-push changes the count and re-arms the scan/re-injection path.
	class PauseMenuEntry
	{
	public:
		// Set the entry label + the overlay view id opened on press. Call once
		// from Runtime::Initialize before the first Tick.
		static void Configure(std::string a_label, std::string a_viewId);

		// Config/MCM gate (osfui.pauseMenuEntry). Runtime syncs it at init and
		// on MCM change; Reconcile is a no-op while disabled.
		static void SetEnabled(bool a_enabled);

		// Game main thread only — called by MainThreadMenuPump after
		// UI_AdvanceActiveMenus each frame: act on a pending click, then keep
		// the entry + click listener present while the pause menu is open.
		static void Reconcile();
	};
}
