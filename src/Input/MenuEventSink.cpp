#include "Input/MenuEventSink.h"

#include "Core/Log.h"
#include "Runtime/Runtime.h"

namespace OSFUI
{
	MenuEventSink    MenuEventSink::s_instance;
	std::atomic_bool MenuEventSink::s_consoleOpen{ false };

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
		// Keep the rare console edge visible in the default log for hotkey diagnosis.
		if (std::string_view{ a_event.menuName } == "Console") {
			s_consoleOpen.store(a_event.opening, std::memory_order_relaxed);
			REX::INFO("MenuEventSink: console {}", a_event.opening ? "opened" : "closed");
		}

		if (a_event.opening) {
			// Force-hide on system transitions to release input before game state becomes invalid.
			const std::string_view name = a_event.menuName;
			if ((name == "LoadingMenu" || name == "MainMenu") && Runtime::Get().IsVisible()) {
				REX::DEBUG("MenuEventSink: '{}' opened -> closing all OSF UI views", name);
				Runtime::Get().EnqueuePresentationRequest(ViewPresentationRequest::CloseAll);
			}
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	bool MenuEventSink::ConsoleOpen()
	{
		return s_consoleOpen.load(std::memory_order_relaxed);
	}
}
