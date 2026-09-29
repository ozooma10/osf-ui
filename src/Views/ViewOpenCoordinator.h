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
	// Runtime-owned. Menu requests live here through first-frame submission.
	// Runtime owns instantiation and presentation; this class never calls the engine, browser or native plugins.
	class ViewOpenCoordinator
	{
	public:
		// Suspended: presentation refuses menus right now; the request stays queued.
		enum class Readiness { Missing, Suspended, Loading, WaitingForInput, InputUnavailable, Ready };
		bool Contains(std::string_view a_view) const;
		enum class Phase { Loading, Rendering, AwaitingSubmission };
		struct MenuOpen
		{
			std::string view;
			double deadline;
			std::uint64_t requestId{}; // Zero for opens without a Settings completion.
			Phase phase{ Phase::Loading };
		};
		MenuOpen* PendingMenu() { return m_menu ? &*m_menu : nullptr; }
		const MenuOpen* PendingMenu() const { return m_menu ? &*m_menu : nullptr; }
		std::optional<MenuOpen> TakeMenu(); // Runtime completes or cleans up the returned operation.

		// Call after instantiation succeeds. Replaces only the pending
		// menu, leaving the currently presented menu and HUD requests untouched.
		// Runtime must finish the previous operation before replacing it.
		void QueueMenu(std::string_view a_view, double a_deadline, std::uint64_t a_requestId = 0);
		// HUDs always pass through the load gate, including startup and host recovery.
		void QueueHud(std::string_view a_view);
		std::vector<std::string> TakeReady(const std::function<Readiness(std::string_view)>& a_readiness);

		bool CancelHud(std::string_view a_view);
		void ClearHuds();

	private:
		std::optional<MenuOpen> m_menu;
		std::unordered_set<std::string> m_huds;
	};
}
