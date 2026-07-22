#include "input/MenuEventSink.h"

#include "core/Log.h"
#include "runtime/Runtime.h"

namespace OSFUI
{
	MenuEventSink       MenuEventSink::s_instance;
	std::atomic_int32_t MenuEventSink::s_openMenus{ 0 };
	std::atomic_bool    MenuEventSink::s_consoleOpen{ false };

	bool MenuEventSink::Install()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			REX::ERROR("MenuEventSink: RE::UI singleton is null; menu events unavailable");
			return false;
		}
		ui->RegisterSink<RE::MenuOpenCloseEvent>(&s_instance);
		REX::INFO("MenuEventSink: registered for MenuOpenCloseEvent");
		return true;
	}

	RE::BSEventNotifyControl MenuEventSink::ProcessEvent(
		const RE::MenuOpenCloseEvent& a_event,
		RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
	{
		// Console edge for the hotkey gameplay gate (MenuMode). INFO on purpose:
		// rare, and the decisive line when triaging "my hotkey fired / didn't
		// fire while the console was up" from a default (non-dev) log.
		if (std::string_view{ a_event.menuName } == "Console") {
			s_consoleOpen.store(a_event.opening, std::memory_order_relaxed);
			REX::INFO("MenuEventSink: console {}", a_event.opening ? "opened" : "closed");
		}

		if (a_event.opening) {
			s_openMenus.fetch_add(1, std::memory_order_relaxed);

			// Force-hide on transition / system menus: the overlay must not
			// linger over a loading screen or the main menu (where the game
			// device and state we read may be invalid), and hiding releases
			// input capture so the game is not left input-frozen across a
			// transition. The user re-opens with the toggle key.
			const std::string_view name = a_event.menuName;
			if ((name == "LoadingMenu" || name == "MainMenu") && Runtime::Get().IsVisible()) {
				REX::INFO("MenuEventSink: '{}' opened -> closing all OSF UI surfaces", name);
				Runtime::Get().EnqueueMenuRequest(Runtime::MenuReq::CloseAll);
			}
		} else {
			// Menus open before registration can close after; don't go negative.
			auto count = s_openMenus.load(std::memory_order_relaxed);
			while (count > 0 && !s_openMenus.compare_exchange_weak(count, count - 1, std::memory_order_relaxed)) {}
		}

		if (Log::DevMode()) {
			REX::DEBUG("MenuEventSink: menu '{}' {} (open count ~{})",
				a_event.menuName, a_event.opening ? "opened" : "closed", s_openMenus.load());
		}
		return RE::BSEventNotifyControl::kContinue;
	}

	std::int32_t MenuEventSink::OpenMenuCount()
	{
		return s_openMenus.load(std::memory_order_relaxed);
	}

	bool MenuEventSink::ConsoleOpen()
	{
		return s_consoleOpen.load(std::memory_order_relaxed);
	}
}
