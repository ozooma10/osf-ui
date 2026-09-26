#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace OSFUI
{
	// Runtime-owned. Owns unpresented open requests, using canonical manifest ids.
	// Runtime owns instantiation and presentation; this class never calls the engine, browser or native plugins.
	class ViewOpenCoordinator
	{
	public:
		// Suspended: presentation refuses menus right now; the request stays queued.
		enum class Readiness { Missing, Suspended, Loading, WaitingForInput, InputUnavailable, Ready };
		bool Contains(std::string_view a_view) const;
		const std::optional<std::string>& PendingMenu() const { return m_menu; }

		// A failed menu must not appear much later after recovery. HUD intent
		// survives failed loads until an explicit close or view teardown.
		void OnLoadFailed(std::string_view a_view);

		// Call after instantiation succeeds. Replaces only the pending
		// menu, leaving the currently presented menu and HUD requests untouched.
		// Returns true when desired presentation can select this menu immediately.
		bool QueueMenu(std::string_view a_view, Readiness a_readiness);
		// HUDs always pass through the load gate, including startup and host recovery.
		void QueueHud(std::string_view a_view);
		std::vector<std::string> TakeReady(const std::function<Readiness(std::string_view)>& a_readiness);

		bool CancelMenu();
		void SuspendMenus(); // host loss/game transition
		bool Cancel(std::string_view a_view);
		void Clear();

	private:
		std::optional<std::string> m_menu;
		std::unordered_set<std::string> m_huds;
	};
}
