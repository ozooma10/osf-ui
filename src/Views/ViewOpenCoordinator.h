#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace OSFUI
{
	// Runtime-owned. Owns unpresented open requests and their timing, using canonical manifest ids.
	// Runtime owns instantiation and presentation; this class never calls the engine, browser or native plugins.
	class ViewOpenCoordinator
	{
	public:
		using Clock = std::chrono::steady_clock;
		// Suspended: presentation refuses menus right now; the request stays queued.
		enum class Readiness { Missing, Suspended, Loading, WaitingForInput, InputUnavailable, Ready };
		struct Timing
		{
			std::string view;
			std::int64_t totalMs, instantiateMs, loadMs, presentMs;
		};

		bool Contains(std::string_view a_view) const;
		const std::optional<std::string>& PendingMenu() const { return m_menu; }

		// Begin before instantiation so its duration belongs to this open request.
		// Repeated requests preserve the first timestamp. Invalid/future timestamps
		// use a_now; callers can pass a clock explicitly for deterministic tests.
		void BeginTiming(std::string_view a_view, bool a_instantiated, std::optional<Clock::time_point> a_requestedAt = std::nullopt, Clock::time_point a_now = Clock::now());
		void OnInstantiated(std::string_view a_view, Clock::time_point a_now = Clock::now());
		void OnLoad(std::string_view a_view, bool a_failed, Clock::time_point a_now = Clock::now());
		std::optional<Timing> FinishTiming(std::string_view a_view, Clock::time_point a_now = Clock::now());
		void CancelTiming(std::string_view a_view);

		// Call after instantiation succeeds. Replaces only the pending
		// menu, leaving the currently presented menu and HUD requests untouched.
		// Returns true when desired presentation can select this menu immediately.
		bool QueueMenu(std::string_view a_view, Readiness a_readiness, std::optional<Clock::time_point> a_requestedAt = std::nullopt, Clock::time_point a_now = Clock::now());
		// HUDs always pass through the load gate, including startup and host recovery.
		void QueueHud(std::string_view a_view);
		std::vector<std::string> TakeReady(const std::function<Readiness(std::string_view)>& a_readiness);

		bool CancelMenu();
		void SuspendMenus(); // host loss/game transition: also discard active-menu timing
		bool Cancel(std::string_view a_view);
		void Clear();

	private:
		struct ColdOpenTiming
		{
			std::string view;
			Clock::time_point requestedAt;
			std::optional<Clock::time_point> instantiatedAt, loadedAt;
		};
		std::optional<std::string> m_menu;
		std::unordered_set<std::string> m_huds;
		std::optional<ColdOpenTiming> m_timing;
	};
}
