#include "input/MainThreadMenuPump.h"

#include "input/FocusMenu.h"
#include "input/MenuMode.h"
#include "input/PauseMenuEntry.h"

#include "core/Log.h"
#include "core/ThreadAffinityProbe.h"

#include "REL/Relocation.h"
#include "REL/Trampoline.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

namespace OSFUI::MainThreadMenuPump
{
	namespace
	{
		// Proven on 1.16.244 (OSF RE ui.menu_flags + crash-stack roots
		// 2026-07-23): 99438 = main-loop UI update 0x141890c60, the function at
		// the root of every engine-side menu/AS3 crash stack — i.e. the thread
		// that owns Scaleform. 130455 = UI_AdvanceActiveMenus 0x142542320; the
		// caller invokes it twice (base+0x228 and base+0x2A1, each E8 rel32
		// followed by a NOP).
		constexpr std::uint64_t kIdUiUpdateCaller = 99438;
		constexpr std::uint64_t kIdAdvanceActiveMenus = 130455;
		constexpr std::size_t   kCallSiteOffsets[2] = { 0x228, 0x2A1 };

		// Prologue of 130455 spills RDX/R8 as GP registers (no XMM), so a
		// 4-pointer forwarding thunk is ABI-safe — same precedent as
		// UiPassSeam::ExecuteFn.
		using AdvanceFn = void* (*)(void*, void*, void*, void*);

		std::atomic<AdvanceFn> g_original{ nullptr };
		std::atomic<bool>      g_installed{ false };
		std::atomic<bool>      g_installTried{ false };

		// Snapshots published after each advance pass. Tri-state: -1 = never
		// published. Freshness guards against a stalled pump (load screens can
		// reroute the UI update) handing Tick a stale answer forever.
		std::atomic<int>           g_focusMenuOpen{ -1 };
		std::atomic<int>           g_anyGameMenuOpen{ -1 };
		std::atomic<std::uint64_t> g_lastPassMs{ 0 };
		constexpr std::uint64_t    kFreshWindowMs = 500;

		[[nodiscard]] std::uint64_t NowMs()
		{
			return static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch())
					.count());
		}

		void* AdvanceThunk(void* a_1, void* a_2, void* a_3, void* a_4)
		{
			const auto original = g_original.load(std::memory_order_relaxed);
			void*      result = original ? original(a_1, a_2, a_3, a_4) : nullptr;

			// Post-advance, on the thread that owns the AS3 VM: every admitted
			// movie has just finished its frame and nothing else is inside the
			// VM. This is the safe window for all engine-UI work.
			//
			// Ground-truth anchor for the thread-affinity probe: whatever thread
			// runs this thunk IS the main thread (devMode only; no-op otherwise).
			ThreadProbe::NoteMainLoop();
			PauseMenuEntry::Reconcile();
			g_focusMenuOpen.store(FocusMenu::IsOpenInEngine() ? 1 : 0, std::memory_order_release);
			g_anyGameMenuOpen.store(MenuMode::AnyGameMenuOpen() ? 1 : 0, std::memory_order_release);
			g_lastPassMs.store(NowMs(), std::memory_order_release);
			return result;
		}

		[[nodiscard]] std::optional<bool> Snapshot(const std::atomic<int>& a_state)
		{
			const auto state = a_state.load(std::memory_order_acquire);
			if (state < 0) {
				return std::nullopt;
			}
			if (NowMs() - g_lastPassMs.load(std::memory_order_acquire) > kFreshWindowMs) {
				return std::nullopt;
			}
			return state != 0;
		}
	}

	bool Install()
	{
		if (g_installTried.exchange(true, std::memory_order_acq_rel)) {
			return g_installed.load(std::memory_order_acquire);
		}

		const REL::Relocation<std::uintptr_t> caller{ REL::ID(kIdUiUpdateCaller) };
		const REL::Relocation<std::uintptr_t> advance{ REL::ID(kIdAdvanceActiveMenus) };

		// Verify both call sites byte-exactly before touching anything: each
		// must be an E8 rel32 whose target is UI_AdvanceActiveMenus. A mismatch
		// means a game-patch layout change or a foreign hook got there first —
		// stay out entirely rather than chain onto an unknown ABI.
		for (const auto offset : kCallSiteOffsets) {
			const auto site = caller.address() + offset;
			std::uint8_t opcode = 0;
			std::int32_t rel = 0;
			std::memcpy(&opcode, reinterpret_cast<const void*>(site), sizeof(opcode));
			std::memcpy(&rel, reinterpret_cast<const void*>(site + 1), sizeof(rel));
			const auto dest = site + 5 + static_cast<std::intptr_t>(rel);
			if (opcode != 0xE8 || dest != advance.address()) {
				REX::WARN("MainThreadMenuPump: call site 0x{:X} does not match E8 -> UI_AdvanceActiveMenus "
						  "(opcode 0x{:02X}, dest 0x{:X}, expected 0x{:X}) — game patch or foreign hook; "
						  "pump NOT installed (pause-menu entry stays uninjected, Tick uses legacy reads)",
					site, opcode, dest, advance.address());
				return false;
			}
		}

		// empty() (capacity == 0) is the "no trampoline" test — NOT
		// allocated_size(), which returns bytes WRITTEN and is 0 for any
		// freshly-reserved trampoline. If SFSE gave us none, self-create one
		// anchored to the call site so AllocTrampoline reserves within ±2GB of
		// it — the E8 rel32 write_call emits must reach the JMP14 island, and
		// the island must reach our thunk. Both call sites share one destination
		// (AdvanceThunk), so a single 14-byte island is used; 512 is headroom.
		auto& trampoline = REL::GetTrampoline();
		if (trampoline.empty()) {
			trampoline.create(512, reinterpret_cast<void*>(caller.address()));
			if (trampoline.empty()) {
				REX::WARN("MainThreadMenuPump: could not allocate a trampoline near the game module; "
						  "pump NOT installed (pause-menu entry stays uninjected, Tick uses legacy reads)");
				return false;
			}
			REX::DEBUG("MainThreadMenuPump: self-created a 512B trampoline near 0x{:X}", caller.address());
		}
		if (trampoline.free_size() < 2 * (sizeof(REL::ASM::JMP14))) {
			REX::WARN("MainThreadMenuPump: trampoline has {} bytes free, need room for a branch island; "
					  "pump NOT installed", trampoline.free_size());
			return false;
		}

		// Publish the forward target before any call site can reach the thunk.
		g_original.store(reinterpret_cast<AdvanceFn>(advance.address()), std::memory_order_release);
		for (const auto offset : kCallSiteOffsets) {
			trampoline.write_call<5>(caller.address() + offset, AdvanceThunk);
		}
		g_installed.store(true, std::memory_order_release);
		REX::INFO("MainThreadMenuPump: armed — engine-UI work now runs post-UI_AdvanceActiveMenus on the "
				  "game main thread (2 call sites hooked in the main-loop UI update)");
		return true;
	}

	bool Installed()
	{
		return g_installed.load(std::memory_order_acquire);
	}

	std::optional<bool> FocusMenuOpenSnapshot()
	{
		return Snapshot(g_focusMenuOpen);
	}

	std::optional<bool> AnyGameMenuOpenSnapshot()
	{
		return Snapshot(g_anyGameMenuOpen);
	}
}
