#include "input/MenuEventSink.h"

#include "core/Log.h"
#include "runtime/Runtime.h"

namespace OSFUI
{
	MenuEventSink       MenuEventSink::s_instance;
	std::atomic_int32_t MenuEventSink::s_openMenus{ 0 };

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
		if (a_event.opening) {
			s_openMenus.fetch_add(1, std::memory_order_relaxed);

			// Force-hide the overlay on transition / system menus: it must not
			// linger over a loading screen or sit at the main menu (where the
			// game device + state we read may not be valid), and hiding releases
			// input capture so the game is never left input-frozen across a
			// transition. The user re-opens with the toggle key afterwards.
			const std::string_view name = a_event.menuName;
			if ((name == "LoadingMenu" || name == "MainMenu") && Runtime::Get().IsVisible()) {
				REX::INFO("MenuEventSink: '{}' opened -> force-hiding overlay", name);
				Runtime::Get().SetVisible(false);
			}
		} else {
			// Menus open before we registered can close after; don't go
			// negative.
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
}
