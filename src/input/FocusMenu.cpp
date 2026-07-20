#include "input/FocusMenu.h"

#include "input/EngineInput.h"

#include "RE/B/BSFixedString.h"
#include "RE/IDs_VTABLE.h"
#include "RE/I/IMenu.h"
#include "RE/S/ScaleformPtr.h"
#include "RE/U/UI.h"
#include "RE/U/UIMessageQueue.h"

#include "core/Log.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace OSFUI
{
	namespace
	{
		// Set once on the load thread (Register), read on the main thread
		// (Open/Close from Runtime::Tick).
		std::atomic_bool g_registered{ false };

		// The hardened creator below builds a fully engine-initialised IMenu (real
		// engine vtable + interned +0xB0 menuName), so the engine's name-keyed menu
		// walk never derefs a null/garbage `this`. This was the headless crash root
		// cause, now dump-confirmed (see FocusMenu.h). Flip live.
		bool g_creatorReady{ true };

		// One interned BSFixedString for the menu name (AddMessage takes a ref).
		const RE::BSFixedString& MenuName()
		{
			static const RE::BSFixedString name{ FocusMenu::MENU_NAME.data() };
			return name;
		}

		// ---- proven construction constants (1.16.244; OSF RE module ui.menu_flags) ----

		// Engine IMenu base-init: void(IMenu* this). Installs the engine's vtables +
		// refcount=1 + uiMovie=null. NOT named in CommonLibSF (the RE::ID::IMenu IDs
		// are {0} placeholders), so it lives here with provenance.
		constexpr std::uint64_t kID_IMenuBaseInit = 130615;  // 0x1425516b0

		// Over-allocate the base IMenu (~0x138) and zero it. The engine takes
		// ownership; we pin the refcount so it is never freed through the Scaleform
		// allocator (an intentional, single per-session leak — the menu lives for
		// the whole session like every native menu).
		constexpr std::size_t kAllocSize = 0x200;
		constexpr std::size_t kVtblSlots = 32;  // covers IMenu vfuncs 0x00..0x1A

		// IMenu field offsets (mirror RE/I/IMenu.h static_asserts).
		constexpr std::size_t kOffRefCount = 0x008;  // u32 Scaleform refcount
		constexpr std::size_t kOffMovie    = 0x088;  // Scaleform::Ptr<Movie> uiMovie
		constexpr std::size_t kOffName     = 0x0B0;  // BSFixedString menuName  <- the crash field
		constexpr std::size_t kOffFlags    = 0x0C0;  // u32 RE::IMenu::Flag
		constexpr std::size_t kOffFlagsUpd = 0x0D2;  // bool flagsUpdated

		std::atomic_bool   g_vtblBuilt{ false };
		// MSVC stores the RTTICompleteObjectLocator* at vtable[-1] (the 8 bytes
		// BEFORE slot 0). The engine dynamic_cast's live menus (__RTDynamicCast),
		// which reads that COL; if it is garbage the cast AVs and is rethrown as
		// "Access violation - no RTTI data!" (exception E06D7363). So reserve one
		// LEADING slot for a copy of the engine's COL and hand the object a vtable
		// pointer of &g_vtableStore[1]. This only matters once the menu is ADMITTED
		// to the active array (Route A) — before that the engine never cast us, so
		// the missing COL was latent.
		void*              g_vtableStore[kVtblSlots + 1]{};
		void** const       g_vtable = &g_vtableStore[1];

		// Engine IMenu base ProcessMessage (primary vtable slot 8, captured in
		// BuildVtable BEFORE the slot is patched). See Thunk_ProcessMessage.
		using ProcessMessageFn = std::int64_t (*)(void*, void*);
		ProcessMessageFn g_baseProcessMessage{ nullptr };

		// ---- vtable thunks (MS x64 ABI matches the vtable thiscall) ----
		const char*   Thunk_GetName(void*) { return FocusMenu::MENU_NAME.data(); }
		const char*   Thunk_GetRootPath(void*) { return ""; }  // web-backed: no .swf root
		std::uint64_t Thunk_GetUnk05(void*) { return 0; }
		bool          Thunk_LoadMovie(void*, bool, bool) { return true; }  // success, no Scaleform movie

		// Handle kShow ourselves; delegate every other message to the engine base.
		//
		// PROVEN RULE (2026-07-02, in-game confirmed — OSF RE ui.imenu_dynamic_cast
		// + memory osf-ui-jank-menu-mode): a fabricated IMenu MUST let the engine
		// base ProcessMessage run the HIDE path, or it corrupts the active menu
		// array. The old thunk blanket-returned 0 for every message, which silently
		// skipped the base's kHide teardown — the engine base ProcessMessage is what
		// REMOVES the menu from the active array (UI+0x430 count / +0x438 data). Note
		// it is NOT the return code: the base also returns 0/kHandled for kHide, the
		// exact value the old no-op returned — it's the base impl's SIDE EFFECTS
		// (array removal + count decrement) that were being skipped. A no-op kHide
		// left a desynced array (stale count / dangling slot); the next menu event's
		// top-modal walk (dynamic_cast<GameMenuBase*> over the array) then hit the
		// bad slot and AV'd on RTTI (E06D7363, ~48s later, dump-confirmed).
		//
		// kShow is the ONE message we must NOT delegate: the engine base returns
		// 1/kIgnore for show (movie-less => refuse), which makes the pump reject
		// admission; Route A admission needs 0/kHandled here. Show-path base side
		// effects are not needed for the movie-less path (proven stable); hide-path
		// side effects ARE.
		std::int64_t Thunk_ProcessMessage(void* a_this, void* a_msg)
		{
			// UIMessageData: vptr @0x00, UI_MESSAGE_TYPE @0x08 (CommonLibSF
			// UIMessageQueue.h). kShow=0, kUpdate=1, kHide=2.
			const auto type = a_msg
			                      ? *reinterpret_cast<const std::uint32_t*>(
			                            reinterpret_cast<const std::uint8_t*>(a_msg) + 0x08)
			                      : 0u;
			if (type == static_cast<std::uint32_t>(RE::UI_MESSAGE_TYPE::kShow)) {
				return 0;  // kHandled — the base's 1/kIgnore would refuse admission (Route A)
			}
			if (g_baseProcessMessage) {
				const auto ret = g_baseProcessMessage(a_this, a_msg);
				if (Log::DevMode()) {
					REX::DEBUG("FocusMenu: ProcessMessage type={} -> engine base ret={}", type, ret);
				}
				return ret;
			}
			return 0;
		}
		// 0x0A (+0x50): the show pump's stack-admission predicate. The engine base
		// (REL::ID 130619) is literally `return uiMovie != nullptr`, and the pump
		// (130449) calls it before inserting the menu into the active array
		// (UI+0x430) — so a movie-less menu was silently never admitted (zero input
		// dispatch, IsMenuOpen false). Forcing true is THE fix (OSF RE
		// ui.menu_movie_load, "Route A": no .swf asset needed). After insertion the
		// engine sets bit6/kAdvancesMovie via vf 0x10, so IsMenuOpen then reflects
		// real array membership. Proven live + stable on 1.16.244.
		bool          Thunk_CanShow(void*) { return true; }

		// Build the patched vtable once: a copy of the real engine IMenu primary
		// vtable so every engine vfunc lands on engine code, with only the six
		// slots we own redirected to our thunks (03/04/05/06/08 + the 0x0A
		// admission predicate). (The C++ FocusMenu vtable routes
		// engine calls through CommonLibSF's {0}-ID relocation thunks => image+0
		// jump => crash; copying the engine vtable is what makes this stable.)
		void BuildVtable()
		{
			if (g_vtblBuilt.load(std::memory_order_acquire)) {
				return;
			}
			static REL::Relocation<std::uintptr_t> engineVtbl{ RE::VTABLE::IMenu[0] };
			const auto* src = reinterpret_cast<void* const*>(engineVtbl.address());
			// Carry the engine's RTTICompleteObjectLocator* (at src[-1]) so a
			// dynamic_cast through our copied primary vtable resolves to the engine
			// IMenu type instead of AV'ing on missing RTTI. See g_vtableStore.
			g_vtableStore[0] = src[-1];
			for (std::size_t i = 0; i < kVtblSlots; ++i) {
				g_vtable[i] = src[i];
			}
			// Capture the engine base ProcessMessage before patching slot 8 —
			// Thunk_ProcessMessage delegates all non-show messages to it.
			g_baseProcessMessage = reinterpret_cast<ProcessMessageFn>(src[8]);
			g_vtable[3] = reinterpret_cast<void*>(&Thunk_GetName);        // 03 GetName
			g_vtable[4] = reinterpret_cast<void*>(&Thunk_GetRootPath);    // 04 GetRootPath
			g_vtable[5] = reinterpret_cast<void*>(&Thunk_GetUnk05);       // 05 GetUnk05
			g_vtable[6] = reinterpret_cast<void*>(&Thunk_LoadMovie);      // 06 LoadMovie
			g_vtable[8] = reinterpret_cast<void*>(&Thunk_ProcessMessage); // 08 ProcessMessage (show->0, rest->engine base; see thunk)
			g_vtable[10] = reinterpret_cast<void*>(&Thunk_CanShow);      // 0A stack-admission predicate (movie-less; see Thunk_CanShow)
			g_vtblBuilt.store(true, std::memory_order_release);
		}
	}

	FocusMenu::FocusMenu()
	{
		// De-jank minimum: cursor only. We deliberately do NOT set kModal.
		// Evidence (OSF RE ui.menu_flags flag table): EVERY world-visible menu
		// (HUDMenu, CursorMenu, FaderMenu, FavoritesMenu, PowersMenu) has kModal
		// CLEAR, and EVERY full-screen menu that blacks out / stops rendering the
		// 3D world (PauseMenu, ContainerMenu, MainMenu, ...) has kModal SET. Once
		// the menu is ADMITTED (Route A), kModal makes the engine treat us as a
		// full application menu and suppress the world render -> opaque black
		// behind the overlay. kModal is NOT needed for input either: ui.menu_input
		// proved input gating is flags bit 4, not kModal. NO pause bits (the sim
		// pause is input/SimPause via the UI pause counter). And NO ShowCursor:
		// bit 3 was only ever a middleman — it makes the cursor-visibility driver
		// open CursorMenu, whose kShow/kHide are what reference-count the OS-cursor
		// release AND draw the frozen center arrow (OSF RE ui.menu_cursor). OSF UI
		// takes the free-cursor reference directly (input/FreeCursor), so with
		// bit 3 clear we get the release with no arrow — and the driver actively
		// kHides any stray CursorMenu while our overlay is the decider menu.
		// SetFlags only writes members (flags @0xC0 + flagsUpdated), safe anytime.
		SetFlags(0);
	}

	RE::Scaleform::Ptr<RE::IMenu>* FocusMenu::Creator(RE::Scaleform::Ptr<RE::IMenu>* a_out)
	{
		// HARDENED creator — the proven, dump-validated recipe (see FocusMenu.h).
		// Builds a fully engine-initialised menu so the engine's name-keyed menu
		// walk (`mov rcx,[rcx+0xB0]`) always reads a live menuName, never null.
		BuildVtable();

		auto* obj = std::calloc(1, kAllocSize);
		if (!obj) {
			// Hand the engine a null Ptr; the open simply fails (no crash).
			*reinterpret_cast<void**>(a_out) = nullptr;
			return a_out;
		}

		// Engine IMenu base-init: installs the engine's own vtables, refcount=1,
		// uiMovie=null. This is what the headless make_shared path skipped.
		static REL::Relocation<void (*)(void*)> baseInit{ REL::ID(kID_IMenuBaseInit) };
		baseInit(obj);

		auto* bytes = reinterpret_cast<std::uint8_t*>(obj);

		// Install our patched primary vtable (engine copy + our 5 slots).
		*reinterpret_cast<void**>(bytes + 0) = &g_vtable[0];

		// Web-backed: keep uiMovie null. The per-frame movie sites all
		// null-guard +0x88 (proven), so movie work is skipped, not dereferenced.
		*reinterpret_cast<void**>(bytes + kOffMovie) = nullptr;

		// THE FIX: construct a valid menuName in place at +0xB0 so the engine's
		// name-keyed dispatch reads a live BSFixedString, not garbage.
		new (bytes + kOffName) RE::BSFixedString(MENU_NAME.data());

		// NO flags at all. Bit 3 (ShowCursor) is no longer needed: it was only a
		// middleman to CursorMenu, whose kShow/kHide reference-count the OS-cursor
		// release AND draw the frozen center arrow — input/FreeCursor now takes
		// the MenuCursor free-cursor reference directly (OSF RE ui.menu_cursor),
		// so bit 3 clear = release with no arrow, and the visibility driver
		// actively kHides any stray CursorMenu while we are the decider menu.
		// No kModal (world-render suppression), no pause bits (input/SimPause).
		// THIS is the write that matters — the C++ ctor never runs for this raw
		// engine-built object, so flag changes must be made HERE (a ctor-only
		// "revert" shipped wrong flags once; don't repeat).
		constexpr std::uint32_t flags = 0;
		*reinterpret_cast<std::uint32_t*>(bytes + kOffFlags) = flags;
		*(bytes + kOffFlagsUpd) = 1;

		// Pin the refcount high so the engine never frees our calloc buffer through
		// the Scaleform allocator (allocator-mismatch guard). Intentional leak: the
		// focus menu lives for the whole session, like every native menu.
		*reinterpret_cast<std::uint32_t*>(bytes + kOffRefCount) = 0x10000000;

		// Level-2 observer (config engineInput): replace the +0x10
		// BSInputEventUser vtable with the patched copy so the engine's per-menu
		// input dispatch is visible to us. No-op unless enabled. Additive: the
		// engine base-init already installed the real receiver vtable + the
		// enabled byte (+0x38=1); we only redirect the six observed slots.
		EngineInput::InstallReceiver(obj);

		// Store the raw object into the out-Ptr WITHOUT going through Ptr(Y*)
		// (which would AddRef via our vtable). Scaleform::Ptr is { T* }.
		*reinterpret_cast<void**>(a_out) = obj;

		REX::INFO("FocusMenu: creator built engine-initialised menu obj=0x{:016X} flags=0x{:08X} (uiMovie=null, name@+0xB0 set)",
			reinterpret_cast<std::uintptr_t>(obj), static_cast<std::uint32_t>(flags));
		return a_out;
	}

	bool FocusMenu::Register()
	{
		if (!g_creatorReady) {
			REX::WARN("FocusMenu: not registering '{}' — creator gated off (g_creatorReady=false).", MENU_NAME);
			return false;
		}
		if (g_registered.load(std::memory_order_acquire)) {
			return true;
		}
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			REX::ERROR("FocusMenu: RE::UI singleton null; cannot register (focus menu inert)");
			return false;
		}

		// Resolve the construction addresses up front so a missing binding fails
		// registration cleanly instead of at first open. RE-verified on 1.16.244;
		// re-verify after any game patch (POST_PATCH_CHECKLIST in OSF RE).
		BuildVtable();

		if (ui->IsMenuRegistered(MenuName())) {
			REX::INFO("FocusMenu: '{}' already registered", MENU_NAME);
			g_registered.store(true, std::memory_order_release);
			return true;
		}
		ui->RegisterMenu(MENU_NAME.data(), &FocusMenu::Creator);
		g_registered.store(true, std::memory_order_release);
		REX::INFO("FocusMenu: registered '{}' (hardened creator: engine base-init + engine vtable copy + "
				  "interned +0xB0 name; opens only when the overlay does). RE-verified 1.16.244.",
			MENU_NAME);
		return true;
	}

	bool FocusMenu::IsRegistered()
	{
		return g_registered.load(std::memory_order_acquire);
	}

	void FocusMenu::Open()
	{
		if (!g_registered.load(std::memory_order_acquire)) {
			return;
		}
		if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
			queue->AddMessage(MenuName(), RE::UI_MESSAGE_TYPE::kShow);
			REX::INFO("FocusMenu: open requested ('{}' kShow)", MENU_NAME);
		} else {
			REX::WARN("FocusMenu: UIMessageQueue singleton null; cannot open");
		}
	}

	void FocusMenu::Close()
	{
		if (!g_registered.load(std::memory_order_acquire)) {
			return;
		}
		if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
			queue->AddMessage(MenuName(), RE::UI_MESSAGE_TYPE::kHide);
			REX::INFO("FocusMenu: close requested ('{}' kHide)", MENU_NAME);
		}
	}

}
