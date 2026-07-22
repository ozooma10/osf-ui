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

#include <cstdint>

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

		// Click dispatch and Runtime::Tick both run on the game main thread.
		bool g_pendingClick{ false };

		// State belongs to a specific movie, not to the delayed open/close event
		// stream. Retaining the movie also makes its address a reliable session key:
		// a rapid close/reopen cannot recycle the old pointer before we observe the
		// replacement.
		struct SessionState
		{
			RE::Scaleform::Ptr<RE::Scaleform::GFx::Movie> movie;
			std::int32_t                                  expectedCount{ -1 };
			bool                                          listenerInstalled{ false };
			bool                                          entryLogged{ false };
			bool                                          failWarned{ false };

			void Reset(RE::Scaleform::Ptr<RE::Scaleform::GFx::Movie> a_movie = nullptr)
			{
				movie = std::move(a_movie);
				expectedCount = -1;
				listenerInstalled = false;
				entryLogged = false;
				failWarned = false;
			}
		};

		SessionState g_session;

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

		// GetMenu alone is not a liveness query: its registration-map slot survives
		// construction and teardown. A movie is callable only while the same menu
		// instance is admitted, marked for advance, and still owns an AS3 root.
		RE::Scaleform::Ptr<RE::IMenu> LivePauseMenu()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return nullptr;
			}

			auto menu = ui->GetMenu(RE::BSFixedString(kMenuName.data()));
			if (!menu || (menu->flags & RE::IMenu::Flag::kAdvancesMovie) == 0 ||
				!menu->uiMovie || !menu->uiMovie->asMovieRoot) {
				return nullptr;
			}

			for (const auto& admitted : ui->menuArray) {
				if (admitted.get() == menu.get()) {
					return menu;
				}
			}
			return nullptr;
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
				// The callback is executing inside a_params.movie's event dispatch,
				// so its arguments belong to the one VM that is known callable for
				// the duration of this call. Do not validate against UI's current
				// PauseMenu before consuming the event: the map can already point at
				// a replacement (or no menu) during a close/reopen transition, and an
				// early return would leak our unknown action id to the engine listener.
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
				if (!event.Invoke("stopImmediatePropagation")) {
					return;
				}

				// A handler is shared across PauseMenu instances. Queue the click only
				// when it came from the movie that is live now.
				const auto menu = LivePauseMenu();
				if (!menu || menu->uiMovie.get() != a_params.movie) {
					return;
				}
				g_pendingClick = true;
			}
		};

		ClickHandler* Handler()
		{
			// CreateFunction releases its reference with the movie. Keep the
			// construction reference so Scaleform never frees a CRT allocation.
			static auto* handler = new ClickHandler();
			return handler;
		}

		// One WARN per pause-menu open when the expected AS3 shape is missing (a
		// UI-overhaul SWF replacing pausemenu.swf can rename clips); silent
		// after that, so the per-tick reconcile can't flood the log.
		void WarnOnce(const char* a_what)
		{
			if (!g_session.failWarned) {
				g_session.failWarned = true;
				REX::WARN("PauseMenuEntry: {} — injection skipped for this pause menu "
						  "(replaced/renamed pausemenu.swf?)", a_what);
			}
		}

		void HandleClick()
		{
			if (!g_pendingClick) {
				return;
			}
			g_pendingClick = false;

			REX::DEBUG("PauseMenuEntry: entry pressed -> closing PauseMenu, opening view '{}'", g_viewId);
			if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
				queue->AddMessage(RE::BSFixedString(kMenuName.data()), RE::UI_MESSAGE_TYPE::kHide);
			} else {
				REX::WARN("PauseMenuEntry: UIMessageQueue singleton null; PauseMenu left open");
			}
			Runtime::Get().EnqueueOpenView(g_viewId);
		}

		void ReconcileList()
		{
			const auto menu = LivePauseMenu();
			if (!menu) {
				g_session.Reset();
				return;
			}
			if (g_session.movie.get() != menu->uiMovie.get()) {
				g_session.Reset(menu->uiMovie);
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

			// Wait for the engine's PauseMenuListData push before injecting: an
			// empty list means OnPauseListDataUpdate hasn't run yet, and anything
			// we add now would be stomped (and briefly selected) a frame later.
			if (count <= 0) {
				return;
			}

			// Steady state: entryCount still matches the shape we last left the
			// list in (our entry included), so skip the per-entry scan. The previous
			// every-tick GetDataForEntry sweep made thousands of needless AS3 invokes
			// per pause session; this keeps steady state to one property read while
			// the liveness gate above closes the actual teardown hole. An engine
			// re-push wiping our entry drops entryCount back to the vanilla count,
			// which re-arms the scan below. (A re-push that happens to land on the
			// same count goes unnoticed — worst case our entry stays missing for the
			// rest of this pause session.)
			if (count == g_session.expectedCount) {
				return;
			}

			// No timing debounce is needed here. Decompiled 1.16.244 AS3 shows
			// PauseMenu.OnPauseListDataUpdate calling PopulateMainList, which calls
			// InitializeEntries -> InvalidateData -> Update synchronously. Runtime's
			// tick and Scaleform dispatch both run on the game main thread, so this
			// code can run before or after that chain, never in its middle. A stable
			// entryCount would not be a completion signal anyway; the live menu gate
			// above is the actual teardown barrier.

			// Install the press listener once per session. It lives on the root
			// (presses bubble up from MainPanel), so the engine re-pushing list
			// data doesn't disturb it.
			if (!g_session.listenerInstalled) {
				RE::Scaleform::GFx::Value fn;
				movieRoot.CreateFunction(&fn, Handler());
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
				g_session.listenerInstalled = true;
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
				if (!newList.PushBack(entryVal)) {
					WarnOnce("could not copy a PauseMenu list entry");
					return;
				}
			}
			if (foundOurs) {
				g_session.expectedCount = count;  // ours survived a shape change; settle on the new count
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
			if (!entry.SetMember("sActionText", label) ||
				!entry.SetMember("uActionType", RE::Scaleform::GFx::Value(kActionId)) ||
				!entry.SetMember("bDisabled", RE::Scaleform::GFx::Value(false)) ||
				!entry.SetMember("bShowSpinner", RE::Scaleform::GFx::Value(false)) ||
				!entry.SetMember("bHasNotification", RE::Scaleform::GFx::Value(false)) ||
				!entry.SetMember("sConfirmText", emptyStr) || !newList.PushBack(entry)) {
				WarnOnce("could not build the MOD MENUS list entry");
				return;
			}

			const RE::Scaleform::GFx::Value args[1] = { newList };
			if (!mainPanel.Invoke("PopulateMainList", nullptr, args, 1)) {
				WarnOnce("MainPanel_mc.PopulateMainList refused the augmented list");
				return;
			}
			// PopulateMainList is synchronous, so this is the count the next tick
			// must observe unless the engine pushes another list.
			g_session.expectedCount = count + 1;
			if (!g_session.entryLogged) {
				g_session.entryLogged = true;
				REX::DEBUG("PauseMenuEntry: '{}' injected into PauseMenu main list ({} vanilla entries)",
					g_label, count);
			}
		}
	}

	void PauseMenuEntry::Configure(std::string a_label, std::string a_viewId)
	{
		g_label = std::move(a_label);
		g_viewId = std::move(a_viewId);
	}

	void PauseMenuEntry::Reconcile()
	{
		HandleClick();
		ReconcileList();
	}
}
