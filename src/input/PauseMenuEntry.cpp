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

namespace OSFUI
{
	namespace
	{
		constexpr std::string_view kMenuName = "PauseMenu";

		// uActionType of the injected entry. Vanilla PMA_* ids are
		// EnumHelper-sequential ints 0..11 (decompiled pausemenu.swf 1.16.244,
		// tmp/pausemenu-re); 100 is far outside that range and, thanks to the
		// priority-1000 listener below, never reaches the engine anyway.
		constexpr std::uint32_t kActionId = 100;

		std::string g_label = "MOD SETTINGS";
		std::string g_viewId = "settings";

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

		// Extract any AS3 numeric (the VM stores ints, uints or Numbers
		// depending on origin) as a double for comparisons.
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

		// The native press callback, installed into the movie via
		// asMovieRoot->CreateFunction + root.addEventListener. Fires on the
		// game's main thread DURING the movie's event dispatch, so it must stay
		// tiny and re-entrancy-free: identify our entry, swallow the event,
		// flag the click for the next Reconcile.
		class ClickHandler final : public RE::Scaleform::GFx::FunctionHandler
		{
		public:
			void Call(const Params& a_params) override
			{
				if (a_params.argCount < 1 || !a_params.args) {
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
				// Ours. Our listener runs FIRST (priority 1000 vs. the menu's
				// own priority-0 listener on the same node), so stopping here
				// keeps PauseMenu from forwarding an unknown actionType to the
				// engine as PauseMenu_StartAction.
				event.Invoke("stopImmediatePropagation");
				g_clicked.store(true, std::memory_order_release);
			}
		};

		// Heap singleton, intentionally never Release()d by us: CreateFunction's
		// function object add-refs it per movie and releases on movie teardown,
		// so our construction ref (1) pins it alive forever — the engine can
		// never drive the count to 0 and free a CRT allocation through the
		// Scaleform allocator (the same allocator-mismatch guard as FocusMenu's
		// pinned refcount).
		ClickHandler* g_handler{ nullptr };

		// Scan pass over the live entryList: count entries and look for ours.
		// Optionally copy every visited entry into a new array being built for
		// PopulateMainList.
		class EntryScan final : public RE::Scaleform::GFx::Value::ArrayVisitor
		{
		public:
			explicit EntryScan(RE::Scaleform::GFx::Value* a_copyInto = nullptr) :
				_copyInto(a_copyInto)
			{}

			void Visit(std::uint32_t, const RE::Scaleform::GFx::Value& a_val) override
			{
				++count;
				RE::Scaleform::GFx::Value action;
				double                    id = -1.0;
				if (a_val.IsObject() && a_val.GetMember("uActionType", &action) &&
					NumericValue(action, id) && id == static_cast<double>(kActionId)) {
					foundOurs = true;
				}
				if (_copyInto) {
					_copyInto->PushBack(a_val);
				}
			}

			std::uint32_t count{ 0 };
			bool          foundOurs{ false };

		private:
			RE::Scaleform::GFx::Value* _copyInto{ nullptr };
		};

		// One WARN per pause-menu open when the expected AS3 shape is missing
		// (a UI-overhaul SWF replacing pausemenu.swf could rename clips); after
		// that stay silent so a per-tick reconcile can't flood the log.
		void WarnOnce(const char* a_what)
		{
			if (!g_failWarned) {
				g_failWarned = true;
				REX::WARN("PauseMenuEntry: {} — injection skipped for this pause menu "
						  "(replaced/renamed pausemenu.swf?)", a_what);
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
		// Act on a pending click first — even if the pause menu already went
		// away for another reason (kHide on a closing menu is harmless).
		if (g_clicked.exchange(false, std::memory_order_acq_rel)) {
			REX::INFO("PauseMenuEntry: entry pressed -> closing PauseMenu, opening view '{}'", g_viewId);
			if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
				queue->AddMessage(RE::BSFixedString(kMenuName.data()), RE::UI_MESSAGE_TYPE::kHide);
			} else {
				REX::WARN("PauseMenuEntry: UIMessageQueue singleton null; PauseMenu left open");
			}
			Runtime::Get().EnqueueOpenView(g_viewId);
		}

		if (!g_pauseMenuOpen.load(std::memory_order_acquire)) {
			return;
		}

		// New pause-menu session: reset per-open state (the menu object — and
		// with it our listener — may have been rebuilt).
		if (const auto gen = g_openGen.load(std::memory_order_acquire); gen != g_handledGen) {
			g_handledGen = gen;
			g_listenerInstalled = false;
			g_entryLogged = false;
			g_failWarned = false;
		}

		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return;
		}
		const auto menu = ui->GetMenu(RE::BSFixedString(kMenuName.data()));
		if (!menu || !menu->uiMovie || !menu->uiMovie->asMovieRoot) {
			return;  // movie not up yet this frame; retry next tick
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
		RE::Scaleform::GFx::Value entryList;
		if (!mainPanel.GetMember("MainList_mc", &mainList) || !mainList.IsObject() ||
			!mainList.GetMember("entryList", &entryList) || !entryList.IsArray()) {
			WarnOnce("MainList_mc/entryList missing under MainPanel_mc");
			return;
		}

		// Wait for the engine's PauseMenuListData push before injecting: an
		// empty list means OnPauseListDataUpdate hasn't run yet, and anything
		// we add now would be stomped (and briefly selected) a frame later.
		EntryScan scan;
		entryList.VisitElements(&scan);
		if (scan.count == 0) {
			return;
		}

		// Install the press listener once per pause-menu session. The listener
		// lives on the ROOT (presses bubble to it from MainPanel), so the
		// engine re-pushing list data doesn't disturb it.
		if (!g_listenerInstalled) {
			if (!g_handler) {
				g_handler = new ClickHandler();
			}
			RE::Scaleform::GFx::Value fn;
			movieRoot.CreateFunction(&fn, g_handler);
			const RE::Scaleform::GFx::Value args[4] = {
				RE::Scaleform::GFx::Value("MainPanel_EntryPress"),
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

		if (scan.foundOurs) {
			return;  // steady state: entry present, nothing to do this tick
		}

		// (Re)inject: rebuild the data array (current entries + ours) and hand
		// it to the menu's own PopulateMainList — the same path the engine's
		// data push takes, so rawEntries/entryList/selection all stay coherent.
		RE::Scaleform::GFx::Value newList;
		movieRoot.CreateArray(&newList);
		EntryScan copy(&newList);
		entryList.VisitElements(&copy);

		RE::Scaleform::GFx::Value entry;
		movieRoot.CreateObject(&entry);
		entry.SetMember("text", RE::Scaleform::GFx::Value(g_label.c_str()));
		entry.SetMember("uActionType", RE::Scaleform::GFx::Value(kActionId));
		entry.SetMember("bDisabled", RE::Scaleform::GFx::Value(false));
		entry.SetMember("sConfirmText", RE::Scaleform::GFx::Value(""));
		newList.PushBack(entry);

		const RE::Scaleform::GFx::Value args[1] = { newList };
		if (!mainPanel.Invoke("PopulateMainList", nullptr, args, 1)) {
			WarnOnce("MainPanel_mc.PopulateMainList refused the augmented list");
			return;
		}
		if (!g_entryLogged) {
			g_entryLogged = true;
			REX::INFO("PauseMenuEntry: '{}' injected into PauseMenu main list ({} vanilla entries)",
				g_label, copy.count);
		}
	}
}
