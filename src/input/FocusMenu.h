#pragma once

#include "RE/I/IMenu.h"
#include "RE/S/ScaleformPtr.h"

namespace OSFUI
{
	// The "de-jank" path (config `focusMenu`, on by default; verified in-game on
	// 1.16.244). A real Starfield IMenu registered onto the engine menu stack so
	// the engine enters menu mode (cursor + modal input ownership + optional
	// pause) instead of relying on the WndProc message-swallow: the world is then
	// gated/paused by the engine and gamepad input no longer leaks past the window
	// hook. See docs/reverse-engineering-notes.md.
	//
	// Registration works: UI::RegisterMenu (130463) interns the name; on
	// AddMessage(kShow) the engine invokes the creator (flags=0x108 =
	// ShowCursor|kModal).
	//
	// A headless base-IMenu crashes: the engine's name-keyed menu walk
	// (UI_MenuNameKeyedDispatch 0x14962b540) does `mov rcx,[rcx+0xB0]` to read
	// menuName on a menu that was never engine-initialised (a make_shared
	// FocusMenu skips the engine base-init, so vtable + refcount + name field are
	// not engine-wired) — AV with RCX=0 on the UI worker thread.
	//
	// So the .cpp Creator (g_creatorReady=true) builds a fully engine-initialised
	// menu: calloc -> engine IMenu base-init (REL::ID 130615, not named in
	// CommonLibSF) -> copy the engine primary vtable (RE::VTABLE::IMenu[0] =
	// REL::ID 475515) with slots 3/4/5/6/8 + 0x0A patched -> construct a valid
	// menuName in place at +0xB0 -> pin the refcount. The +0xB0 name is the guard
	// against that crash. uiMovie stays null (web-backed; the per-frame movie
	// sites null-guard +0x88, so no .swf is required). Open/close go through
	// UIMessageQueue::AddMessage (130659).
	//
	// Stack admission ("Route A"): the show pump only inserts a menu into the
	// active array (UI+0x430) if primary vf 0x0A (+0x50) returns true, and the
	// engine base for it is `return uiMovie != null` — a movie-less menu is
	// registered but never admitted, so no input dispatch and IsMenuOpen==false.
	// The 0x0A thunk (Thunk_CanShow -> true) admits us with no asset; after
	// insertion the engine sets bit6 via vf 0x10, so IsMenuOpen then reflects real
	// membership. GetRootPath()="" is correct: it is an AS display path consumed
	// by LoadMovie, not the movie file.
	//
	// This class satisfies RE::IMenu's pure virtuals so the type compiles, but the
	// live object the engine receives is the raw engine-built one from the static
	// Creator, not a C++ FocusMenu instance, so this class carries only static
	// entry points and named constants; the runtime uses the copied engine vtable.
	//
	// Flag bits (1.16.244): bit3 ShowCursor, bit8 kModal, bit27 freeze-frame
	// latch. The real kPausesGame is bit 1, not bit 27.
	class FocusMenu final
	{
	public:
		static constexpr std::string_view MENU_NAME = "OSFUI_FocusMenu";

		// Not set: on an admitted menu this summons the engine's Scaleform cursor
		// arrow, which freezes at screen center (the WndProc swallow starves it of
		// mouse input). The hardware cursor is the pointer.
		static constexpr std::uint32_t kFlagShowCursor = 1u << 3;
		// Top-of-stack modal selector, not set: once the menu is admitted kModal
		// makes the engine treat us as a full application menu and stop rendering
		// the 3D world behind the overlay (opaque black). World-visible engine
		// menus have it clear, full-screen ones have it set. Not needed for input
		// either — that gate is bit 4. Named constant for reference only.
		static constexpr std::uint32_t kFlagModal      = 1u << 8;
		// Cosmetic freeze-frame/letterbox latch only; the real pause flag is bit 1,
		// and OSF UI pauses via UI::ModifyMenuPauseCounter in input/SimPause rather
		// than menu flags. Only consulted when the menu is the top kModal menu (we
		// are not). Letterbox, if ever wanted: menu->Unk0E(&menuName, bool) with
		// this bit (CLSF ID::IMenu::Unk0E{130622}, live-proven — but
		// latch-on-non-modal is an unnatural state; soak before shipping). Named
		// constant for reference only.
		static constexpr std::uint32_t kFlagFreezeFrameLatch = 1u << 27;

		// Platform-facing API; call from the game main thread.

		// Register the menu name + creator with RE::UI. Idempotent; call at
		// kPostPostDataLoad (the UI singleton exists by then). Returns false (and
		// logs) if the UI singleton is unavailable.
		static bool Register();

		// Open/close the menu via the UI message queue. Game main thread only —
		// Runtime drives these from Tick(); UIMessageQueue is not safe to poke from
		// the WndProc/input thread. No-op until Register() succeeds.
		static void Open();
		static void Close();

		// True once Register() has run successfully this session.
		[[nodiscard]] static bool IsRegistered();

		// Engine truth: whether the menu is in the admitted (active) menu array —
		// the state that gates input, unlike the fire-and-forget Open/Close
		// requests above. Same admitted-array walk MenuMode uses;
		// RE::UI::IsMenuOpen is not trusted (it returned false for an open Console
		// in a live run). Main thread.
		[[nodiscard]] static bool IsOpenInEngine();


		// Creator handed to RE::UI::RegisterMenu (UIMenuEntry::Create_t).
		static RE::Scaleform::Ptr<RE::IMenu>* Creator(RE::Scaleform::Ptr<RE::IMenu>* a_out);
	};
}
