#pragma once

#include "RE/I/IMenu.h"
#include "RE/S/ScaleformPtr.h"

namespace OSFUI
{
	// The "de-jank" path (config `focusMenu`, on by default; verified in-game
	// on 1.16.244).
	//
	// A real Starfield IMenu that OSF UI registers and pushes onto the engine
	// menu stack so the ENGINE enters menu mode (cursor + modal input ownership
	// + optional pause) instead of the WndProc message-swallow the overlay uses
	// today. With the engine aware a menu is open, the world is gated/paused by
	// the engine itself and gamepad input no longer leaks past the window hook
	// (see docs/reverse-engineering-notes.md §4 and the jank diagnosis).
	//
	// PROBE RESULT — live on 1.16.244 (OSF RE module ui.menu_flags +
	// custom-imenu-registration), crash root cause DUMP-confirmed:
	//   * Registration WORKS. UI::RegisterMenu (130463) interns the name; on
	//     AddMessage(kShow) the engine invokes the creator (logged flags=0x108 =
	//     ShowCursor|kModal).
	//   * The headless base-IMenu crash is the engine's name-keyed menu walk
	//     (UI_MenuNameKeyedDispatch 0x14962b540) doing `mov rcx,[rcx+0xB0]` —
	//     reading menuName — on a menu object that was never engine-initialised
	//     (a make_shared FocusMenu skips the engine base-init, so its vtable +
	//     refcount + name field are not engine-wired). Trainwreck
	//     2026-06-13-18-31-35.log: AV, RCX=0, R13=(UI*), UI worker thread.
	//
	// FIX (implemented in the .cpp Creator, g_creatorReady=true): build a fully
	// engine-initialised menu with the proven recipe — calloc -> engine IMenu
	// base-init (REL::ID 130615, not named in CommonLibSF) -> COPY the engine
	// primary vtable (RE::VTABLE::IMenu[0] = REL::ID 475515) with slots
	// 3/4/5/6/8 + 0x0A patched -> construct a valid menuName in place at +0xB0 ->
	// pin the refcount. The +0xB0 name is the specific guard against the crash;
	// uiMovie stays null (web-backed; the per-frame movie sites null-guard
	// +0x88, so no .swf is required). Open/close CALLS are proven
	// (UIMessageQueue::AddMessage 130659).
	//
	// STACK ADMISSION (OSF RE ui.menu_movie_load, 2026-07-01, "Route A"): the show
	// pump only inserts a menu into the active array (UI+0x430) if primary vf 0x0A
	// (+0x50) returns true, and the engine base for it is `return uiMovie != null`
	// — so a movie-less menu is registered but NEVER admitted, hence no input
	// dispatch and IsMenuOpen==false. The 0x0A thunk (Thunk_CanShow -> true) is
	// what admits us with no asset; after insertion the engine sets bit6 via vf
	// 0x10, so IsMenuOpen then reflects real membership. GetRootPath()="" is
	// CORRECT (it is an AS display path consumed by LoadMovie, not the movie file).
	//
	// NOTE: this class still satisfies RE::IMenu's pure virtuals so the type
	// compiles, but the LIVE object the engine receives is the raw engine-built
	// one from the static Creator — NOT a C++ FocusMenu instance. The members
	// below are the type contract; the runtime menu uses the copied engine vtable.
	//
	// Flag bits are proven (OSF RE modules ui.menu_flags + ui.menu_pause):
	// bit3 ShowCursor, bit8 kModal, bit27 freeze-frame latch (NOT the pause —
	// the real kPausesGame is bit 1, corrected 2026-07-02). The proven values
	// are named locally below; the CLSF mirror enum now matches.
	class FocusMenu final : public RE::IMenu
	{
	public:
		static constexpr std::string_view MENU_NAME = "OSFUI_FocusMenu";

		// Proven RE::IMenu::Flag bits (1.16.244).
		// NOT SET since 2026-07-02: on an ADMITTED menu this summons the engine's
		// Scaleform cursor arrow, which freezes at screen center (the WndProc
		// swallow starves it of mouse input). The hardware cursor is the pointer.
		static constexpr std::uint32_t kFlagShowCursor = 1u << 3;
		// top-of-stack modal selector. INTENTIONALLY NOT SET: once the menu is
		// admitted, kModal makes the engine treat us as a full application menu and
		// STOP rendering the 3D world behind the overlay (opaque black). Every
		// world-visible engine menu has it clear; every full-screen one has it set
		// (ui.menu_flags). Not needed for input either (the gate is bit 4, not this
		// — ui.menu_input). Kept as a named constant for reference only.
		static constexpr std::uint32_t kFlagModal      = 1u << 8;
		// Bit 27 = the cosmetic freeze-frame/letterbox latch ONLY (renamed from
		// "kPausesGame" 2026-07-02 after OSF RE ui.menu_pause disproved the
		// 06-13 pause claim — the real pause flag is bit 1, and OSF UI pauses
		// via UI::ModifyMenuPauseCounter in input/SimPause instead of menu
		// flags). Only consulted when the menu is the top kModal menu (we are
		// not). Letterbox, if ever wanted: menu->Unk0E(&menuName, bool) with
		// this bit (CLSF ID::IMenu::Unk0E{130622}, live-proven — but
		// latch-on-non-modal is an unnatural state; soak before shipping).
		// Named constant kept for reference only.
		static constexpr std::uint32_t kFlagFreezeFrameLatch = 1u << 27;

		// ---- platform-facing API (call from the game main thread) ----

		// Register the menu name + creator with RE::UI. Idempotent. Safe to call
		// once at kPostPostDataLoad (the UI singleton exists by then). Returns
		// false (and logs) if the UI singleton is unavailable.
		static bool Register();

		// Open/close the menu via the UI message queue. MUST run on the game main
		// thread — Runtime drives these from Tick(); UIMessageQueue is not safe to
		// poke from the WndProc/input thread. No-op until Register() succeeds.
		static void Open();
		static void Close();

		// True once Register() has run successfully this session.
		[[nodiscard]] static bool IsRegistered();

		// ENGINE truth: whether the menu is currently in the admitted (active)
		// menu array — the state that actually gates input, unlike the
		// fire-and-forget Open/Close requests above. Same admitted-array walk
		// MenuMode uses (RE::UI::IsMenuOpen is not trusted: it returned false
		// for an open Console in a 2026-07-18 live run). Main thread.
		[[nodiscard]] static bool IsOpenInEngine();

		// ---- RE::IMenu contract ----
		FocusMenu();

		// Creator handed to RE::UI::RegisterMenu (UIMenuEntry::Create_t).
		static RE::Scaleform::Ptr<RE::IMenu>* Creator(RE::Scaleform::Ptr<RE::IMenu>* a_out);

		// pure virtuals (vfuncs 03/04/05)
		const char*   GetName() const override { return MENU_NAME.data(); }
		const char*   GetRootPath() const override { return ""; }  // web-backed: no .swf root (the runtime object's slot-4 is Thunk_GetRootPath, also "")
		std::uint64_t GetUnk05() override { return 0; }

		// IMenu also derives BSTEventSink<UpdateSceneRectEvent>; satisfy its pure
		// ProcessEvent. We don't react to scene-rect changes.
		RE::BSEventNotifyControl ProcessEvent(
			const RE::UpdateSceneRectEvent&,
			RE::BSTEventSource<RE::UpdateSceneRectEvent>*) override
		{
			return RE::BSEventNotifyControl::kContinue;
		}

		// Allocate on the Scaleform heap like every engine menu (the open path
		// frees menus through that heap; a global-new'd object would mismatch).
		SF_SCALEFORM_HEAP_REDEFINE_NEW(FocusMenu);
	};
}
