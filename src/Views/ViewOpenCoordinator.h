#pragma once

#include <functional>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace OSFUI
{
	// Runtime-owned; never calls the engine, browser or native plugins.
	// Preparing a frame does not grant input; an ordinary open consumes it.
	class ViewOpenCoordinator
	{
	public:
		// Keep requests queued while menus cannot open.
		enum class Readiness { Missing, Suspended, Loading, WaitingForInput, InputUnavailable, Ready };
		bool Contains(std::string_view a_view) const;
		enum class Phase { Loading, Rendering, Ready };
		struct MenuOpen
		{
			std::string view;
			double deadline;
			std::uint64_t requestId{}; // Nonzero waits for this request's after-close callback.
			Phase phase{ Phase::Loading };
		};
		MenuOpen* PendingMenu() { return m_menu ? &*m_menu : nullptr; }
		const MenuOpen* PendingMenu() const { return m_menu ? &*m_menu : nullptr; }
		std::optional<MenuOpen> TakeMenu(); // Runtime completes or cleans up the returned operation.
		std::optional<MenuOpen> TakeReadyMenu(std::string_view a_view, std::uint64_t a_requestId = 0);

		// Call after creating the view and finishing the previous pending menu.
		void QueueMenu(std::string_view a_view, double a_deadline, std::uint64_t a_requestId = 0);
		// HUDs must finish loading, including at startup and after browser recovery.
		void QueueHud(std::string_view a_view);
		std::vector<std::string> TakeReady(const std::function<Readiness(std::string_view)>& a_readiness);

		bool CancelHud(std::string_view a_view);

	private:
		std::optional<MenuOpen> m_menu;
		std::unordered_set<std::string> m_huds;
	};
}
