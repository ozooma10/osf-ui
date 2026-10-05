#pragma once

#include <atomic>

#include "RE/E/Events.h"
#include "RE/U/UI.h"

namespace OSFUI
{
	class MenuEventSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		// First runtime tick after kPostPostDataLoad.
		static bool Install();

		RE::BSEventNotifyControl ProcessEvent(
			const RE::MenuOpenCloseEvent& a_event,
			RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

		// Any thread.
		[[nodiscard]] static bool ChargenOpen();
		[[nodiscard]] static bool TransitionOpen();

	private:
		MenuEventSink() = default;

		static MenuEventSink    s_instance;
		static std::atomic_bool s_chargenOpen;
		static std::atomic_bool s_loadingOpen;
		static std::atomic_bool s_mainMenuOpen;
	};
}
