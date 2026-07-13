#pragma once

#include <string>

namespace OSFUI
{
	// EXPERIMENTAL (config.pauseMenuEntry). Injects a "MOD SETTINGS" entry into
	// the ENGINE's PauseMenu main list at runtime and opens a configured overlay
	// view (default "settings") when it is pressed.
	//
	// Mechanism — live Scaleform manipulation, NO SWF edit (so no conflict with
	// UI-overhaul mods, and pausemenu_lrg.swf large-font mode is covered for
	// free since it shares the same AS3 classes):
	//
	//   * The pause menu's main list is DATA-DRIVEN FROM NATIVE: the engine
	//     pushes PauseMenuListData.aPauseMenuList (entries {text, uActionType,
	//     bDisabled, sConfirmText}) into PauseMenu.OnPauseListDataUpdate ->
	//     MainPanel.PopulateMainList. We read the live entryList, append our
	//     entry (uActionType 100 — vanilla PMA_* ids are EnumHelper-sequential
	//     0..11) and re-invoke PopulateMainList through the movie's GFx Value
	//     API. PopulateMainList preserves the selection by action id, so a
	//     mid-session re-add doesn't jump the cursor.
	//   * Presses bubble from MainPanel as CustomEvent "MainPanel_EntryPress"
	//     {entryAction}; the PauseMenu root forwards them to the engine as
	//     PauseMenu_StartAction. We addEventListener on the root at priority
	//     1000 (the menu's own listener is priority 0 on the same node) with a
	//     CreateFunction-wrapped native callback: for OUR action id it calls
	//     stopImmediatePropagation() — the engine never sees the unknown
	//     actionType — and flags the click for the next Tick.
	//   * On click (main thread, next Reconcile): close the pause menu via the
	//     engine's own channel (UIMessageQueue kHide — the proven FocusMenu
	//     pattern) and queue the overlay view open through the normal menu
	//     policy path (Runtime::EnqueueOpenView).
	//
	// The engine may re-push PauseMenuListData at any time (it re-runs
	// PopulateMainList and wipes the injected entry), so Reconcile re-checks
	// presence every tick while the pause menu is open — a scan of ~10 small
	// GFx reads at pause-menu framerate.
	//
	// Source of truth for the AS3 structure: pausemenu.swf 1.16.244 decompiled
	// with JPEXS 2026-07-13 (kept at tmp/pausemenu-re next to this repo); see
	// docs/reverse-engineering-notes.md. Pending in-game verification: that the
	// SFSE per-frame task keeps ticking while PauseMenu is open (console menu:
	// observed yes), and the close->open handoff feel.
	//
	// All GFx access runs on the game's main thread (Runtime::Tick); no engine
	// hooks — GetMenu/GetRootPath/GFx Value calls are documented CommonLibSF
	// surface backed by AddressLib IDs.
	class PauseMenuEntry
	{
	public:
		// Set the entry label + the overlay view id opened on press. Call once
		// from Runtime::Initialize before the first Tick.
		static void Configure(std::string a_label, std::string a_viewId);

		// MenuEventSink edge for the engine "PauseMenu" (any thread; atomics
		// only). opening=false also resets the per-open injection state.
		static void NotifyPauseMenu(bool a_opening);

		// Main thread, every Tick (gated on config.pauseMenuEntry by the
		// caller): act on a pending click, then keep the entry + click listener
		// present while the pause menu is open.
		static void Reconcile();
	};
}
