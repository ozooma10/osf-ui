#include "Input/FocusMenu.h"

#include "RE/B/BSFixedString.h"
#include "RE/U/UI.h"
#include "RE/U/UIMessageQueue.h"

#include <atomic>

namespace OSFUI
{
	namespace
	{
		// Register, Open and Close run from Runtime::Update; engine input also reads these flags.
		std::atomic_bool g_registered{ false };
		std::atomic_bool g_gamepadCapture{ false };

		const RE::BSFixedString& MenuName()
		{
			// Process lifetime: a BSFixedString destructor calls the engine.
			static auto* const name = new RE::BSFixedString(FocusMenu::MENU_NAME.data());
			return *name;
		}

		void StopEvent(const RE::InputEvent* a_event)
		{
			const_cast<RE::InputEvent*>(a_event)->status = RE::InputEvent::Status::kStop;
		}
	}

	FocusMenu::FocusMenu()
	{
		menuName = MENU_NAME.data();
		// No ShowCursor (engine arrow would freeze at centre), no kModal (blacks out the world),
		// no kPausesGame (SimPause drives the pause counter directly).
		flags = 0;
		flagsUpdated = true;
	}

	RE::UI_MESSAGE_RESULT FocusMenu::ProcessMessage(RE::UIMessageData& a_message)
	{
		// The base answers kIgnore to kShow for a menu without a movie, which refuses stack admission.
		// Every other message must reach the base: kHide's active-array teardown lives there, and
		// skipping it leaves UI::menuArray desynced.
		if (a_message.type == RE::UI_MESSAGE_TYPE::kShow) {
			return RE::UI_MESSAGE_RESULT::kHandled;
		}
		return RE::IMenu::ProcessMessage(a_message);
	}

	void FocusMenu::OnThumbstickEvent(const RE::ThumbstickEvent* a_event)
	{
		if (a_event && g_gamepadCapture.load(std::memory_order_relaxed)) {
			StopEvent(reinterpret_cast<const RE::InputEvent*>(a_event));
		}
	}

	void FocusMenu::OnButtonEvent(const RE::ButtonEvent* a_event)
	{
		if (a_event && a_event->deviceType == RE::InputEvent::DeviceType::kGamepad &&
			g_gamepadCapture.load(std::memory_order_relaxed)) {
			StopEvent(a_event);
		}
	}

	RE::Scaleform::Ptr<RE::IMenu> FocusMenu::Creator()
	{
		return RE::Scaleform::Ptr<RE::IMenu>{ new FocusMenu() };
	}

	bool FocusMenu::Register()
	{
		if (g_registered.load(std::memory_order_acquire)) {
			return true;
		}
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			REX::ERROR("FocusMenu: RE::UI singleton null; cannot register (focus menu inert)");
			return false;
		}
		if (!ui->IsMenuRegistered(MenuName())) {
			ui->RegisterMenu(MENU_NAME.data(), &FocusMenu::Creator);
		}
		if (!ui->IsMenuRegistered(MenuName())) {
			REX::ERROR("FocusMenu: RegisterMenu('{}') did not take; focus menu inert", MENU_NAME);
			return false;
		}
		g_registered.store(true, std::memory_order_release);
		REX::INFO("FocusMenu: registered '{}' (movie-less GameMenuBase; opens only when the overlay does)", MENU_NAME);
		return true;
	}

	bool FocusMenu::IsRegistered()
	{
		return g_registered.load(std::memory_order_acquire);
	}

	bool FocusMenu::IsOpenInEngine()
	{
		if (!g_registered.load(std::memory_order_acquire)) {
			return false;
		}
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return false;
		}
		for (const auto& menu : ui->menuArray) {
			if (menu && menu->menuName == MENU_NAME) {
				return true;
			}
		}
		return false;
	}

	void FocusMenu::SetGamepadCapture(bool a_capture)
	{
		if (g_gamepadCapture.exchange(a_capture, std::memory_order_relaxed) != a_capture) {
			REX::DEBUG("FocusMenu: gamepad capture gate {}", a_capture ? "ON" : "off");
		}
	}

	void FocusMenu::Open()
	{
		if (!g_registered.load(std::memory_order_acquire)) {
			return;
		}
		if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
			queue->AddMessage(MenuName(), RE::UI_MESSAGE_TYPE::kShow);
			REX::DEBUG("FocusMenu: open requested ('{}' kShow)", MENU_NAME);
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
			REX::DEBUG("FocusMenu: close requested ('{}' kHide)", MENU_NAME);
		}
	}
}
