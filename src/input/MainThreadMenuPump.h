#pragma once

namespace OSFUI::MainThreadMenuPump
{
	// Runs engine-UI work on the thread that actually owns Scaleform.
	//
	// SFSE TaskInterface callbacks do NOT run on the game's main thread:
	// crash-stack analysis (2026-07-23, three trainwreck logs) shows its pump
	// executing inside a render-graph pass on a job-pool worker, while the engine
	// advances menus / AS3 / UI data models on the main thread (WinMain
	// 0x1418814c0 -> main loop -> UI update 0x141890c60). Runtime::Tick used to
	// run directly from that callback, making its UI access the proven root of
	// the pause-menu CTD family. Tick now marshals wholesale through BSService,
	// but PauseMenuEntry still needs this stricter post-AS3-advance window.
	//
	// The pump hooks the two direct call sites of UI_AdvanceActiveMenus
	// (AddrLib 130455) inside the main-loop UI update (AddrLib 99438) — both
	// verified byte-exact (E8 rel32 + NOP) before patching, fail-closed
	// otherwise. Post-advance, on the owning thread, it drives
	// PauseMenuEntry::Reconcile() after each advance, when every admitted movie
	// has finished its frame and nothing else is inside the AS3 VM.
	//
	// Install once from the kPostPostDataLoad handler (after UiLayoutGuard, so
	// a game-patch layout drift disables this too). Requires the SFSE
	// trampoline (main.cpp Init allocates it).
	bool Install();
}
