#pragma once

#include "RE/E/Events.h"
#include "RE/U/UI.h"

namespace OSFUI
{
	// Observes the game's menu open/close stream via the documented
	// CommonLibSF API (RE::UI is a BSTEventSource<MenuOpenCloseEvent>;
	// no hooking involved). Currently observational: it logs traffic in dev
	// mode and tracks a simple open-menu count that later drives overlay
	// policy (force-hide during menus, focus rules — Phase 4).
	class MenuEventSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		// Registers on RE::UI::GetSingleton(); call once the UI singleton
		// exists (kPostPostDataLoad). Returns false (and logs) if it doesn't.
		// Events fired before registration are missed by design.
		static bool Install();

		RE::BSEventNotifyControl ProcessEvent(
			const RE::MenuOpenCloseEvent& a_event,
			RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

		// Best-effort count of menus opened since registration. Not a full
		// menu-mode model; do not build pause logic on it yet.
		[[nodiscard]] static std::int32_t OpenMenuCount();

		// The dev console is open (tracked off its open/close edges, same as
		// the PauseMenu edge above). Feeds MenuMode::AnyGameMenuOpen — the
		// console is kModal-clear so the flag walk alone misses it. Any thread.
		[[nodiscard]] static bool ConsoleOpen();

	private:
		MenuEventSink() = default;

		static MenuEventSink     s_instance;
		static std::atomic_int32_t s_openMenus;
		static std::atomic_bool  s_consoleOpen;
	};
}
