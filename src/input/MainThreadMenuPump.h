#pragma once

#include <optional>

namespace OSFUI::MainThreadMenuPump
{
	// Runs engine-UI work on the thread that actually owns Scaleform.
	//
	// SFSE TaskInterface tasks (Runtime::Tick) do NOT run on the game's main
	// thread: crash-stack analysis (2026-07-23, three trainwreck logs) shows the
	// task pump executing inside a render-graph pass on a job-pool worker, while
	// the engine advances menus / AS3 / UI data models on the main thread
	// (WinMain 0x1418814c0 -> main loop -> UI update 0x141890c60). Any
	// ui->GetMenu / menuArray walk / GFx Invoke from Tick therefore races the
	// AS3 VM — the proven root of the pause-menu CTD family (NaN-boxed atom
	// deref at +333AB2E with our frames on the stack; engine-side +3337690 /
	// +254089D faults without them, FG on or off).
	//
	// The pump hooks the two direct call sites of UI_AdvanceActiveMenus
	// (AddrLib 130455) inside the main-loop UI update (AddrLib 99438) — both
	// verified byte-exact (E8 rel32 + NOP) before patching, fail-closed
	// otherwise. Post-advance, on the owning thread, it:
	//   * drives PauseMenuEntry::Reconcile() (all AS3 + menuArray access), and
	//   * publishes menu-state snapshots so Runtime::Tick can read engine UI
	//     state without touching RE::UI cross-thread.
	//
	// Install once from the kPostPostDataLoad handler (after UiLayoutGuard, so
	// a game-patch layout drift disables this too). Requires the SFSE
	// trampoline (main.cpp Init allocates it).
	bool Install();
	bool Installed();

	// Frame-old engine-UI state published by the last pump pass. nullopt when
	// the pump is not installed or the snapshot is stale (no pass within the
	// freshness window — e.g. during load screens); callers fall back to their
	// legacy direct read in that case.
	std::optional<bool> FocusMenuOpenSnapshot();
	std::optional<bool> AnyGameMenuOpenSnapshot();
}
