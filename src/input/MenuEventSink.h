#pragma once

#include "RE/E/Events.h"
#include "RE/U/UI.h"

namespace OSFUI
{
	// Observes the game's menu open/close stream (RE::UI is a
	// BSTEventSource<MenuOpenCloseEvent>; no hooking involved).
	class MenuEventSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		// Registers on RE::UI::GetSingleton(); call once the UI singleton exists
		// (kPostPostDataLoad). Returns false (and logs) if it doesn't. Events
		// fired before registration are missed.
		static bool Install();

		RE::BSEventNotifyControl ProcessEvent(
			const RE::MenuOpenCloseEvent& a_event,
			RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

		// Dev console open, tracked off its open/close edges. Feeds
		// MenuMode::AnyGameMenuOpen — the console is kModal-clear, so the flag
		// walk alone misses it. Any thread.
		[[nodiscard]] static bool ConsoleOpen();

	private:
		MenuEventSink() = default;

		static MenuEventSink     s_instance;
		static std::atomic_int32_t s_openMenus;
		static std::atomic_bool  s_consoleOpen;
	};
}
