#include "Input/PauseMenuEntry.h"

#include "RE/B/BSFixedString.h"
#include "RE/I/IMenu.h"
#include "RE/S/ScaleformGFxASMovieRootBase.h"
#include "RE/S/ScaleformGFxFunctionHandler.h"
#include "RE/S/ScaleformGFxMovie.h"
#include "RE/S/ScaleformGFxValue.h"
#include "RE/U/UI.h"
#include "RE/U/UIMessageQueue.h"

#include "Core/Log.h"
#include "Runtime/Runtime.h"

#include <atomic>
#include <cstdint>

namespace OSFUI
{
	namespace
	{
		constexpr std::string_view kMenuName = "PauseMenu";

		// uActionType of injected entry. vanilla ids are 0..11 so 100 is outside range.
		constexpr std::uint32_t kActionId = 100;

		constexpr std::string_view kLabel = "MOD SETTINGS";
		constexpr std::string_view kViewId = "osfui/settings";

		bool g_pendingClick{ false };

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

		SessionState& Session()
		{
			static SessionState* const session = new SessionState;
			return *session;
		}

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

		RE::Scaleform::Ptr<RE::IMenu> LivePauseMenu()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return nullptr;
			}

			auto menu = ui->GetMenu(RE::BSFixedString(kMenuName.data()));
			if (!menu || (menu->flags & RE::IMenu::Flag::kAdvancesMovie) == 0 || !menu->uiMovie || !menu->uiMovie->asMovieRoot) {
				return nullptr;
			}

			for (const auto& admitted : ui->menuArray) {
				if (admitted.get() == menu.get()) {
					return menu;
				}
			}
			return nullptr;
		}

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
				if (!event.Invoke("stopImmediatePropagation")) {
					return;
				}

				const auto menu = LivePauseMenu();
				if (!menu || menu->uiMovie.get() != a_params.movie) {
					return;
				}
				g_pendingClick = true;
			}
		};

		ClickHandler* Handler()
		{
			static auto* handler = new ClickHandler();
			return handler;
		}

		void WarnOnce(const char* a_what)
		{
			if (!Session().failWarned) {
				Session().failWarned = true;
				REX::WARN("PauseMenuEntry: {} - injection skipped for this pause menu (replaced/renamed pausemenu.swf?)", a_what);
			}
		}

		void HandleClick()
		{
			if (!g_pendingClick) {
				return;
			}
			g_pendingClick = false;

			REX::DEBUG("PauseMenuEntry: entry pressed -> closing PauseMenu, opening view '{}'", kViewId);
			if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
				queue->AddMessage(RE::BSFixedString(kMenuName.data()), RE::UI_MESSAGE_TYPE::kHide);
			} else {
				REX::WARN("PauseMenuEntry: UIMessageQueue singleton null; PauseMenu left open");
			}
			Runtime::Get().EnqueueOpenView(std::string(kViewId));
		}

		void ReconcileList()
		{
			const auto menu = LivePauseMenu();
			if (!menu) {
				Session().Reset();
				return;
			}
			if (Session().movie.get() != menu->uiMovie.get()) {
				Session().Reset(menu->uiMovie);
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

			RE::Scaleform::GFx::Value countVal;
			double                    countNum = 0.0;
			if (!mainList.GetMember("entryCount", &countVal) || !NumericValue(countVal, countNum)) {
				WarnOnce("MainList_mc.entryCount unreadable");
				return;
			}
			const auto count = static_cast<std::int32_t>(countNum);

			if (count <= 0) {
				return;
			}

			if (count == Session().expectedCount) {
				return;
			}

			if (!Session().listenerInstalled) {
				RE::Scaleform::GFx::Value fn;
				movieRoot.CreateFunction(&fn, Handler());

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
				Session().listenerInstalled = true;
			}

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
				Session().expectedCount = count;  // ours survived a shape change; settle on the new count
				return;
			}

			RE::Scaleform::GFx::Value entry;
			movieRoot.CreateObject(&entry);

			RE::Scaleform::GFx::Value label;
			movieRoot.CreateString(&label, kLabel.data());
			RE::Scaleform::GFx::Value emptyStr;
			movieRoot.CreateString(&emptyStr, "");
			if (!entry.SetMember("sActionText", label) || !entry.SetMember("uActionType", RE::Scaleform::GFx::Value(kActionId)) ||
				!entry.SetMember("bDisabled", RE::Scaleform::GFx::Value(false)) || !entry.SetMember("bShowSpinner", RE::Scaleform::GFx::Value(false)) ||
				!entry.SetMember("bHasNotification", RE::Scaleform::GFx::Value(false)) || !entry.SetMember("sConfirmText", emptyStr) || !newList.PushBack(entry)) {
				WarnOnce("could not build the MOD SETTINGS list entry");
				return;
			}

			const RE::Scaleform::GFx::Value args[1] = { newList };
			if (!mainPanel.Invoke("PopulateMainList", nullptr, args, 1)) {
				WarnOnce("MainPanel_mc.PopulateMainList refused the augmented list");
				return;
			}
			Session().expectedCount = count + 1;
			if (!Session().entryLogged) {
				Session().entryLogged = true;
				REX::DEBUG("PauseMenuEntry: '{}' injected into PauseMenu main list ({} vanilla entries)", kLabel, count);
			}
		}
	}

	void PauseMenuEntry::Reconcile()
	{
		HandleClick();
		ReconcileList();
	}
}
