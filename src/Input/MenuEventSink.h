#pragma once

#include <atomic>

#include "RE/E/Events.h"
#include "RE/U/UI.h"

namespace OSFUI
{
	// Observe RE::UI's native MenuOpenCloseEvent source without hooking.
	class MenuEventSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		// Register on the first main-thread tick after kPostPostDataLoad.
		static bool Install();

		RE::BSEventNotifyControl ProcessEvent(
			const RE::MenuOpenCloseEvent& a_event,
			RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

		// Any-thread console edge used because its kModal-clear flag escapes the menu-mode walk.
		[[nodiscard]] static bool ConsoleOpen();
		// Any-thread semantic edge. Runtime consumes it on the game main thread.
		[[nodiscard]] static bool ChargenOpen();

	private:
		MenuEventSink() = default;

		static MenuEventSink    s_instance;
		static std::atomic_bool s_consoleOpen;
		static std::atomic_bool s_chargenOpen;
	};
}
