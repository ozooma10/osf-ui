#include "input/FocusMenu.h"

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

		// Pause policy for the NEXT open + the live object (SetPausesGame). The
		// creator does NOT run on every open (the registration map caches
		// instances — the 2026-07-01 probe saw 3 instances across 5 opens), so
		// the desired flags must be patched onto the live object too, not just
		// the creation template. A stale g_liveMenu after the engine re-creates
		// is harmless: our objects are refcount-pinned (never freed), so the old
		// pointer stays valid and the write is simply ineffective.
		std::atomic_bool   g_wantPause{ false };
		std::atomic<void*> g_liveMenu{ nullptr };

		// The full flag set for the current pause policy. NO kModal ever — see
		// the ctor comment (it suppresses the world render once admitted).
		std::uint32_t DesiredFlags()
		{
			return FocusMenu::kFlagShowCursor |
			       (g_wantPause.load(std::memory_order_relaxed) ? FocusMenu::kFlagPausesGame : 0u);
		}

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

		// ---- vtable thunks (MS x64 ABI matches the vtable thiscall) ----
		const char*   Thunk_GetName(void*) { return FocusMenu::MENU_NAME.data(); }
		const char*   Thunk_GetRootPath(void*) { return ""; }  // Ultralight-backed: no .swf root
		std::uint64_t Thunk_GetUnk05(void*) { return 0; }
		bool          Thunk_LoadMovie(void*, bool, bool) { return true; }  // success, no Scaleform movie
		std::int64_t  Thunk_ProcessMessage(void*, void*) { return 0; }     // no-op; must NOT return 1 for the show msg (the pump then REFUSES admission)
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
			g_vtable[3] = reinterpret_cast<void*>(&Thunk_GetName);        // 03 GetName
			g_vtable[4] = reinterpret_cast<void*>(&Thunk_GetRootPath);    // 04 GetRootPath
			g_vtable[5] = reinterpret_cast<void*>(&Thunk_GetUnk05);       // 05 GetUnk05
			g_vtable[6] = reinterpret_cast<void*>(&Thunk_LoadMovie);      // 06 LoadMovie
			g_vtable[8] = reinterpret_cast<void*>(&Thunk_ProcessMessage); // 08 ProcessMessage
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
		// proved input gating is flags bit 4, not kModal. Pause is per-view via
		// SetPausesGame (kFlagPausesGame, driven off the manifest's pausesGame).
		// SetFlags only writes members (flags @0xC0 + flagsUpdated), safe anytime.
		SetFlags(kFlagShowCursor);
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

		// Ultralight-backed: keep uiMovie null. The per-frame movie sites all
		// null-guard +0x88 (proven), so movie work is skipped, not dereferenced.
		*reinterpret_cast<void**>(bytes + kOffMovie) = nullptr;

		// THE FIX: construct a valid menuName in place at +0xB0 so the engine's
		// name-keyed dispatch reads a live BSFixedString, not garbage.
		new (bytes + kOffName) RE::BSFixedString(MENU_NAME.data());

		// Cursor (+ pause when the top view asks for it) — NO kModal (it makes the
		// engine suppress the 3D world once we are admitted => opaque black behind
		// the overlay; see the ctor for the flag-table evidence). The ctor never
		// runs here (this is a raw engine-built object, not a C++ FocusMenu).
		const auto flags = DesiredFlags();
		*reinterpret_cast<std::uint32_t*>(bytes + kOffFlags) = flags;
		*(bytes + kOffFlagsUpd) = 1;

		// Pin the refcount high so the engine never frees our calloc buffer through
		// the Scaleform allocator (allocator-mismatch guard). Intentional leak: the
		// focus menu lives for the whole session, like every native menu.
		*reinterpret_cast<std::uint32_t*>(bytes + kOffRefCount) = 0x10000000;

		// Store the raw object into the out-Ptr WITHOUT going through Ptr(Y*)
		// (which would AddRef via our vtable). Scaleform::Ptr is { T* }.
		*reinterpret_cast<void**>(a_out) = obj;
		g_liveMenu.store(obj, std::memory_order_release);

		REX::INFO("FocusMenu: creator built engine-initialised menu obj=0x{:016X} flags=0x{:08X} (uiMovie=null, name@+0xB0 set)",
			reinterpret_cast<std::uintptr_t>(obj), flags);
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

	void FocusMenu::SetPausesGame(bool a_pause)
	{
		const bool changed = g_wantPause.exchange(a_pause, std::memory_order_relaxed) != a_pause;
		// Patch the live object too: the creator only runs when the engine builds
		// a fresh instance, and the registration map caches instances across
		// opens — template-only would leave a cached menu on the old policy.
		if (auto* obj = static_cast<std::uint8_t*>(g_liveMenu.load(std::memory_order_acquire))) {
			*reinterpret_cast<std::uint32_t*>(obj + kOffFlags) = DesiredFlags();
			*(obj + kOffFlagsUpd) = 1;
		}
		if (changed) {
			REX::INFO("FocusMenu: pausesGame -> {} (takes effect on next open)", a_pause);
		}
	}
}
