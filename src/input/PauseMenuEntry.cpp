#include "input/PauseMenuEntry.h"

#include "RE/B/BSFixedString.h"
#include "RE/I/IMenu.h"
#include "RE/S/ScaleformGFxASMovieRootBase.h"
#include "RE/S/ScaleformGFxFunctionHandler.h"
#include "RE/S/ScaleformGFxMovie.h"
#include "RE/S/ScaleformGFxValue.h"
#include "RE/U/UI.h"
#include "RE/U/UIMessageQueue.h"

#include "core/Log.h"
#include "runtime/Runtime.h"

#include <atomic>
#include <cstdint>
#include <excpt.h>

namespace OSFUI
{
	namespace
	{
		constexpr std::string_view kMenuName = "PauseMenu";

		// uActionType of the injected entry. Vanilla PMA_* ids are
		// EnumHelper-sequential ints 0..11 (decompiled pausemenu.swf 1.16.244);
		// 100 is outside that range and, thanks to the priority-1000 listener
		// below, never reaches the engine anyway.
		constexpr std::uint32_t kActionId = 100;

		std::string g_label = "MOD MENUS";
		std::string g_viewId = "osfui/settings";

		// Open/close edge state (MenuEventSink thread -> Reconcile main thread).
		// The generation counter makes per-open state (listener installed, log
		// latches) self-reset even across a close+reopen between two ticks.
		std::atomic_bool          g_pauseMenuOpen{ false };
		std::atomic_uint32_t      g_openGen{ 0 };
		std::atomic_bool          g_clicked{ false };

		// Main-thread-only per-open state, keyed to g_openGen.
		std::uint32_t g_handledGen{ 0 };
		bool          g_listenerInstalled{ false };
		bool          g_entryLogged{ false };
		bool          g_failWarned{ false };
		std::int32_t  g_expectedCount{ -1 };  // entryCount we last left the list at (ours included); -1 = not established
		std::int32_t  g_lastCount{ -1 };      // entryCount seen the previous tick; used to debounce an in-flight re-push

		// The VM stores ints, uints or Numbers depending on origin; normalise to
		// double for comparisons.
		bool NumericValue(const RE::Scaleform::GFx::Value& a_val, double& a_out)
		{
			using Type = RE::Scaleform::GFx::Value::ValueType;
			switch (a_val.GetType()) {
			case Type::kInt:
				a_out = a_val.GetInt();
				return true;
			case Type::kUInt:
				a_out = a_val.GetUInt();
				return true;
			case Type::kNumber:
				a_out = a_val.GetNumber();
				return true;
			default:
				return false;
			}
		}

		// The engine's own per-frame liveness signal. kAdvancesMovie (bit 6,
		// 0x40) is set at active-array INSERTION — after LoadMovie has built the
		// movie + AS3 root — and cleared the instant the engine removes the menu
		// on teardown (UI_AdvanceActiveMenus gates the advance on this exact bit).
		// A LIVE read each tick therefore tracks "genuinely open & being
		// advanced," unlike RE::UI::GetMenu, which is a header reimplementation
		// returning the raw registration-map slot (UI+0x470) — non-null through
		// BOTH construction and teardown, so it cannot see the transition windows.
		//
		// This is the fix for the close-event-lag hole: MenuEventSink's
		// opening=false edge (g_pauseMenuOpen) trails real teardown by hundreds of
		// ms (field repro: fault ~485ms after close began), so g_pauseMenuOpen is
		// NOT a safe teardown barrier. bit 6 + array membership go false the frame
		// the engine removes the menu, independent of when the close event fires.
		//
		// Returns true only when it is safe to touch the movie's AS3 side this
		// tick. Caller must read menu->flags LIVE (never cache) every tick.
		// CAVEAT: bit 6 stays SET during an in-place PauseMenuListData re-push
		// (the list rebuilds while the menu is fully open); that window is covered
		// by the count-gate + debounce in ReconcileList, not by this signal.
		bool IsLiveAdvancing(RE::UI* a_ui, const RE::IMenu* a_menu)
		{
			if (!a_ui || !a_menu) {
				return false;
			}
			// Live advance gate: clears on teardown ahead of the close event.
			if ((a_menu->flags & RE::IMenu::Flag::kAdvancesMovie) == 0) {
				return false;
			}
			// Admission ground truth: pointer identity in UI->menuArray (UI+0x430),
			// the array bit 6 rides on. Also rejects a stale registration-map slot
			// left pointing at a different/dying instance.
			bool admitted = false;
			for (const auto& m : a_ui->menuArray) {
				if (m.get() == a_menu) {
					admitted = true;
					break;
				}
			}
			if (!admitted) {
				return false;
			}
			// Movie + AS3 root constructed. Belt for the one unproven ordering
			// caveat (whether bit 6 can outlive asMovieRoot on teardown): if the
			// root is already gone we bail here regardless of the flag.
			return a_menu->uiMovie && a_menu->uiMovie->asMovieRoot;
		}

		// Native press callback, installed via asMovieRoot->CreateFunction +
		// root.addEventListener. Fires on the game's main thread inside the
		// movie's event dispatch, so it must stay tiny and re-entrancy-free:
		// identify our entry, swallow the event, flag the click for Reconcile.
		class ClickHandler final : public RE::Scaleform::GFx::FunctionHandler
		{
		public:
			void Call(const Params& a_params) override
			{
				if (a_params.argCount < 1 || !a_params.args) {
					return;
				}
				// Same live-advance gate as Reconcile: never run AS3 interop
				// (GetMember / stopImmediatePropagation Invoke) on a movie the
				// engine is tearing down. This callback normally only fires while
				// the menu is live, but it runs UNGUARDED in the engine event pump
				// (outside Reconcile's SEH belt), so a stray dispatch mid-teardown
				// must drop rather than dispatch into a dying VM.
				auto* ui = RE::UI::GetSingleton();
				if (!ui || !IsLiveAdvancing(ui, ui->GetMenu(RE::BSFixedString(kMenuName.data())).get())) {
					return;
				}
				auto& event = a_params.args[0];
				if (!event.IsObject()) {
					return;
				}
				RE::Scaleform::GFx::Value eventParams;
				if (!event.GetMember("params", &eventParams) || !eventParams.IsObject()) {
					return;
				}
				RE::Scaleform::GFx::Value action;
				double                    id = -1.0;
				if (!eventParams.GetMember("entryAction", &action) || !NumericValue(action, id)) {
					return;
				}
				if (id != static_cast<double>(kActionId)) {
					return;  // a vanilla entry — leave the event alone
				}
				// Ours. This listener runs first (priority 1000 vs. the menu's
				// own priority-0 listener on the same node), so stopping here
				// keeps PauseMenu from forwarding an unknown actionType to the
				// engine as PauseMenu_StartAction.
				event.Invoke("stopImmediatePropagation");
				g_clicked.store(true, std::memory_order_release);
			}
		};

		// Heap singleton, never Release()d by us: CreateFunction's function
		// object add-refs per movie and releases on movie teardown, so the
		// construction ref (1) pins it forever. Keeps the engine from driving
		// the count to 0 and freeing a CRT allocation through the Scaleform
		// allocator (same allocator-mismatch guard as FocusMenu's pinned ref).
		ClickHandler* g_handler{ nullptr };

		// One WARN per pause-menu open when the expected AS3 shape is missing (a
		// UI-overhaul SWF replacing pausemenu.swf can rename clips); silent
		// after that, so the per-tick reconcile can't flood the log.
		void WarnOnce(const char* a_what)
		{
			if (!g_failWarned) {
				g_failWarned = true;
				REX::WARN("PauseMenuEntry: {} — injection skipped for this pause menu "
						  "(replaced/renamed pausemenu.swf?)", a_what);
			}
		}

		// Diagnostic-only latch (see Reconcile). Demoted from "the fix" — the real
		// containment is IsLiveAdvancing + the re-push debounce, engineered so this
		// never trips. It stays only so an unmodelled fault still self-disables the
		// entry and leaves one telemetry line; a trip means the gate has a hole, not
		// that the fuse saved us (catching an engine-AS3-VM fault corrupts the VM and
		// hangs, reproduced 2026-07-22). History: field report 2026-07-20 (vanilla
		// pausemenu.swf, no other mods) — null method-slot dispatch under
		// GetDataForEntry, Starfield.exe+333E929, `call [r10]`, r10=0/-2.
		bool g_faulted{ false };

		// SEH filter: contain access violations only; anything else (including
		// MSVC C++ exceptions, code 0xE06D7363) keeps propagating. Literal
		// EXCEPTION_ACCESS_VIOLATION value so this file stays Windows.h-free
		// (its ERROR macro collides with REX::ERROR).
		int FaultFilter(unsigned long a_code) noexcept
		{
			return a_code == 0xC0000005uL ? EXCEPTION_EXECUTE_HANDLER :
											EXCEPTION_CONTINUE_SEARCH;
		}

		// Out of line so Reconcile's __except block holds no C++ temporaries
		// (C2712: a __try function must not require object unwinding).
		void NoteFault()
		{
			g_faulted = true;
			REX::ERROR("PauseMenuEntry: access violation inside PauseMenu AS3 interop — "
					   "entry injection disabled for this session");
		}

		// Click delivery: engine UI message queue + our own view queue, no AS3
		// interop — kept outside the SEH guard so a pressed entry still works
		// even on the tick that trips (or has tripped) the fault fuse.
		void HandleClick()
		{
			// Act on a pending click first, even if the pause menu already went
			// away for another reason (kHide on a closing menu is harmless).
			if (!g_clicked.exchange(false, std::memory_order_acq_rel)) {
				return;
			}
			REX::INFO("PauseMenuEntry: entry pressed -> closing PauseMenu, opening view '{}'", g_viewId);
			if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
				queue->AddMessage(RE::BSFixedString(kMenuName.data()), RE::UI_MESSAGE_TYPE::kHide);
			} else {
				REX::WARN("PauseMenuEntry: UIMessageQueue singleton null; PauseMenu left open");
			}
			Runtime::Get().EnqueueOpenView(g_viewId);
		}

		// Everything that reaches into the movie's AS3 side. Runs under the
		// SEH guard in Reconcile.
		void ReconcileList()
		{
			if (!g_pauseMenuOpen.load(std::memory_order_acquire)) {
				return;
			}

			// New session: reset per-open state; the menu object (and with it our
			// listener) may have been rebuilt.
			if (const auto gen = g_openGen.load(std::memory_order_acquire); gen != g_handledGen) {
				g_handledGen = gen;
				g_listenerInstalled = false;
				g_entryLogged = false;
				g_failWarned = false;
				g_expectedCount = -1;
				g_lastCount = -1;
			}

			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return;
			}
			// The single safety gate: the menu must be engine-admitted AND being
			// advanced THIS tick (kAdvancesMovie live + menuArray identity + AS3
			// root present). Bails during the loading and teardown transition
			// windows the old GetMenu/uiMovie/asMovieRoot chain could not see —
			// the whole ReconcileList body below runs synchronously with no engine
			// interleave, so one read here makes every AS3 touch that follows safe
			// from those windows. `menu` (a Scaleform::Ptr) also holds a ref for
			// the rest of this call.
			const auto menu = ui->GetMenu(RE::BSFixedString(kMenuName.data()));
			if (!IsLiveAdvancing(ui, menu.get())) {
				return;  // loading, tearing down, or not admitted this frame
			}
			auto& movieRoot = *menu->uiMovie->asMovieRoot;

			RE::Scaleform::GFx::Value rootObj;
			if (!movieRoot.GetVariable(&rootObj, menu->GetRootPath()) || !rootObj.IsObject()) {
				return;  // root display object not ready yet; retry next tick
			}
			RE::Scaleform::GFx::Value mainPanel;
			if (!rootObj.GetMember("MainPanel_mc", &mainPanel) || !mainPanel.IsObject()) {
				WarnOnce("MainPanel_mc missing on the PauseMenu root");
				return;
			}
			RE::Scaleform::GFx::Value mainList;
			if (!mainPanel.GetMember("MainList_mc", &mainList) || !mainList.IsObject()) {
				WarnOnce("MainList_mc missing under MainPanel_mc");
				return;
			}

			// Read the list through BSScrollingContainer's public surface:
			// entryCount + GetDataForEntry(i). The `entryList` getter is protected
			// in AS3 and invisible to GFx GetMember (verified live 2026-07-13:
			// GetMember("entryList") failed on the real movie).
			RE::Scaleform::GFx::Value countVal;
			double                    countNum = 0.0;
			if (!mainList.GetMember("entryCount", &countVal) || !NumericValue(countVal, countNum)) {
				WarnOnce("MainList_mc.entryCount unreadable");
				return;
			}
			const auto count = static_cast<std::int32_t>(countNum);
			const auto prevCount = g_lastCount;
			g_lastCount = count;  // updated every tick (even under the gates below) so the debounce sees a re-push

			// Wait for the engine's PauseMenuListData push before injecting: an
			// empty list means OnPauseListDataUpdate hasn't run yet, and anything
			// we add now would be stomped (and briefly selected) a frame later.
			if (count <= 0) {
				return;
			}

			// Steady state: entryCount still matches the shape we last left the
			// list in (our entry included), so skip the per-entry scan. This gate
			// is the actual fix for the 2026-07-20 field CTD: the previous
			// every-tick GetDataForEntry sweep was thousands of AS3 invokes per
			// pause session, and one of them landed mid list-rebuild on a null
			// method slot. An engine re-push wiping our entry drops entryCount
			// back to the vanilla count, which re-arms the scan below. (A re-push
			// that happens to land on the same count goes unnoticed — worst case
			// our entry stays missing for the rest of this pause session.)
			if (count == g_expectedCount) {
				return;
			}

			// Debounce the re-push settle window — the ONE crash window the
			// liveness gate can't cover, because bit 6 stays set while the engine
			// rebuilds PauseMenuListData in place on a fully-open menu. A rebuild
			// can present a transient entryCount for a frame; require the same
			// count on two consecutive ticks before we scan (GetDataForEntry) or
			// re-inject (PopulateMainList), so those fl_events-dispatching invokes
			// never land mid-rebuild on a half-built entry clip (the +333E929 null
			// method-slot fault). Costs at most one extra tick (~16ms); self-heals.
			if (count != prevCount) {
				return;  // count still settling; re-check next tick
			}

			// Install the press listener once per session. It lives on the root
			// (presses bubble up from MainPanel), so the engine re-pushing list
			// data doesn't disturb it.
			if (!g_listenerInstalled) {
				if (!g_handler) {
					g_handler = new ClickHandler();
				}
				RE::Scaleform::GFx::Value fn;
				movieRoot.CreateFunction(&fn, g_handler);
				// Managed VM string for the event type too (same raw-kString
				// hazard as the entry label: a mis-read type makes the listener
				// silently never match).
				RE::Scaleform::GFx::Value eventType;
				movieRoot.CreateString(&eventType, "MainPanel_EntryPress");
				const RE::Scaleform::GFx::Value args[4] = {
					eventType,
					fn,
					RE::Scaleform::GFx::Value(false),                 // useCapture
					RE::Scaleform::GFx::Value(std::int32_t{ 1000 }),  // priority: run before the menu's own listener
				};
				if (!rootObj.Invoke("addEventListener", nullptr, args, 4)) {
					WarnOnce("root.addEventListener(MainPanel_EntryPress) failed");
					return;
				}
				g_listenerInstalled = true;
			}

			// Scan for our entry while copying the current entries into a fresh
			// array for a potential re-populate. Reached only when the list shape
			// changed (first tick of an open, or after an engine re-push) — the
			// count-gate above keeps this off the steady-state path.
			RE::Scaleform::GFx::Value newList;
			movieRoot.CreateArray(&newList);
			bool foundOurs = false;
			for (std::int32_t i = 0; i < count; ++i) {
				RE::Scaleform::GFx::Value index(i);
				RE::Scaleform::GFx::Value entryVal;
				if (!mainList.Invoke("GetDataForEntry", &entryVal, &index, 1) || !entryVal.IsObject()) {
					WarnOnce("MainList_mc.GetDataForEntry failed");
					return;
				}
				RE::Scaleform::GFx::Value action;
				double                    id = -1.0;
				if (entryVal.GetMember("uActionType", &action) && NumericValue(action, id) &&
					id == static_cast<double>(kActionId)) {
					foundOurs = true;
				}
				newList.PushBack(entryVal);
			}
			if (foundOurs) {
				g_expectedCount = count;  // ours survived a shape change; settle on the new count
				return;
			}

			// (Re)inject through the menu's own PopulateMainList — the same path the
			// engine's data push takes, so rawEntries/entryList/selection stay
			// coherent.
			RE::Scaleform::GFx::Value entry;
			movieRoot.CreateObject(&entry);
			// The label field is `sActionText`, not the BSContainerEntry base's
			// `text`: MainPanelListEntry overrides SetEntryText and reads
			// {sActionText, bDisabled, bShowSpinner, bHasNotification}. That class
			// lives in mainpanel.swf, a runtime-loaded sub-SWF sharing the app
			// domain (pausemenu.swf alone doesn't show it; using `text` renders a
			// blank row). Strings must be managed VM strings (CreateString), the
			// GFx4/AS3 form for values crossing into AS3 objects.
			RE::Scaleform::GFx::Value label;
			movieRoot.CreateString(&label, g_label.c_str());
			RE::Scaleform::GFx::Value emptyStr;
			movieRoot.CreateString(&emptyStr, "");
			entry.SetMember("sActionText", label);
			entry.SetMember("uActionType", RE::Scaleform::GFx::Value(kActionId));
			entry.SetMember("bDisabled", RE::Scaleform::GFx::Value(false));
			entry.SetMember("bShowSpinner", RE::Scaleform::GFx::Value(false));
			entry.SetMember("bHasNotification", RE::Scaleform::GFx::Value(false));
			entry.SetMember("sConfirmText", emptyStr);
			newList.PushBack(entry);

			const RE::Scaleform::GFx::Value args[1] = { newList };
			if (!mainPanel.Invoke("PopulateMainList", nullptr, args, 1)) {
				WarnOnce("MainPanel_mc.PopulateMainList refused the augmented list");
				return;
			}
			// Steady state from the next tick on is one entryCount read against
			// this value. If PopulateMainList applies a frame late, the next tick
			// mismatches, rescans, finds ours and settles — self-healing.
			g_expectedCount = count + 1;
			if (!g_entryLogged) {
				g_entryLogged = true;
				REX::INFO("PauseMenuEntry: '{}' injected into PauseMenu main list ({} vanilla entries)",
					g_label, count);
			}
		}
	}

	void PauseMenuEntry::Configure(std::string a_label, std::string a_viewId)
	{
		g_label = std::move(a_label);
		g_viewId = std::move(a_viewId);
	}

	void PauseMenuEntry::NotifyPauseMenu(bool a_opening)
	{
		if (a_opening) {
			g_openGen.fetch_add(1, std::memory_order_acq_rel);
		}
		g_pauseMenuOpen.store(a_opening, std::memory_order_release);
	}

	void PauseMenuEntry::Reconcile()
	{
		HandleClick();
		if (g_faulted) {
			return;
		}
		// SEH latch, DIAGNOSTIC-ONLY — no longer the containment strategy. The
		// live-advance gate (kAdvancesMovie + menuArray identity) plus the
		// re-push debounce are what keep us from ever invoking into a
		// tearing-down or mid-rebuild VM, so this should now never trip. It stays
		// only to record a NoteFault line and self-disable if some unmodelled
		// window still exists: catching the AV here does NOT recover the game
		// (the fault is inside the engine AS3 VM; unwinding ~12 non-/EHa frames
		// skips GFx::Value dtors and corrupts the VM -> hang, reproduced
		// 2026-07-22). A tripped fuse is a signal to fix the gate, not a fix.
		__try {
			ReconcileList();
		} __except (FaultFilter(GetExceptionCode())) {
			NoteFault();
		}
	}
}
