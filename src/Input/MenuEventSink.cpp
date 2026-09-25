#include "Input/MenuEventSink.h"

#include "API/PapyrusApi.h"
#include "Core/Log.h"
#include "RE/C/ChargenMenu.h"

namespace OSFUI
{
	MenuEventSink    MenuEventSink::s_instance;
	std::atomic_bool MenuEventSink::s_chargenOpen{ false };
	std::atomic_bool MenuEventSink::s_loadingOpen{ false };
	std::atomic_bool MenuEventSink::s_mainMenuOpen{ false };

	bool MenuEventSink::Install()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			REX::ERROR("MenuEventSink: RE::UI singleton is null; menu events unavailable");
			return false;
		}
		ui->RegisterSink<RE::MenuOpenCloseEvent>(&s_instance);
		s_loadingOpen.store(ui->IsMenuOpen(RE::BSFixedString{ "LoadingMenu" }));
		s_mainMenuOpen.store(ui->IsMenuOpen(RE::BSFixedString{ "MainMenu" }));
		const bool chargenOpen = ui->IsMenuOpen(RE::BSFixedString{ RE::ChargenMenu::MENU_NAME });
		s_chargenOpen.store(chargenOpen, std::memory_order_release);
		REX::INFO("MenuEventSink: registered for MenuOpenCloseEvent (ChargenMenu {})",
			chargenOpen ? "open" : "closed");
		return true;
	}

	RE::BSEventNotifyControl MenuEventSink::ProcessEvent(
		const RE::MenuOpenCloseEvent& a_event,
		RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
	{
		const std::string_view name = a_event.menuName;
		// Keep the rare console edge visible in the default log for hotkey diagnosis.
		if (name == "Console") {
			REX::INFO("MenuEventSink: console {}", a_event.opening ? "opened" : "closed");
		}
		if (name == RE::ChargenMenu::MENU_NAME) {
			const bool changed = s_chargenOpen.exchange(
				a_event.opening, std::memory_order_acq_rel) != a_event.opening;
			if (changed) {
				REX::INFO("MenuEventSink: ChargenMenu {} -> fixed-aspect Scaleform mode {}",
					a_event.opening ? "opened" : "closed",
					a_event.opening ? "requested" : "released");
			}
		}

		if (name == "LoadingMenu") s_loadingOpen.store(a_event.opening, std::memory_order_release);
		if (name == "MainMenu") {
			const bool wasOpen = s_mainMenuOpen.exchange(a_event.opening, std::memory_order_acq_rel);
			if (a_event.opening && !wasOpen) API::Papyrus::OnMainMenuOpened();
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	bool MenuEventSink::TransitionOpen()
	{
		return s_loadingOpen.load(std::memory_order_acquire) || s_mainMenuOpen.load(std::memory_order_acquire);
	}

	bool MenuEventSink::ChargenOpen()
	{
		return s_chargenOpen.load(std::memory_order_acquire);
	}
}
