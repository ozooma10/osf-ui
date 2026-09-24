#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OSFUI
{
	// Main-thread only. Owns unpresented open requests and their timing, using
	// canonical manifest ids. Runtime owns preflight callbacks, instantiation and
	// presentation; this class never calls the engine, browser or native plugins.
	class ViewOpenCoordinator
	{
	public:
		using Clock = std::chrono::steady_clock;
		enum class Readiness { Missing, Loading, WaitingForInput, InputUnavailable, Ready };
		struct Timing
		{
			std::string view;
			std::int64_t totalMs, instantiateMs, loadMs, presentMs;
		};

		bool Contains(std::string_view a_view) const;
		const std::optional<std::string>& PendingMenu() const { return _menu; }

		// Begin before instantiation so its duration belongs to this open request.
		// Repeated requests preserve the first timestamp. Invalid/future timestamps
		// use a_now; callers can pass a clock explicitly for deterministic tests.
		void BeginTiming(std::string_view a_view, bool a_instantiated,
			std::optional<Clock::time_point> a_requestedAt = std::nullopt,
			Clock::time_point a_now = Clock::now());
		void OnInstantiated(std::string_view a_view, Clock::time_point a_now = Clock::now());
		void OnLoad(std::string_view a_view, bool a_failed, Clock::time_point a_now = Clock::now());
		std::optional<Timing> FinishTiming(std::string_view a_view, Clock::time_point a_now = Clock::now());
		void CancelTiming(std::string_view a_view);

		// Call after preflight and instantiation succeed. Replaces only the pending
		// menu, leaving the currently presented menu and HUD requests untouched.
		// Returns true when this menu can be presented immediately.
		bool QueueMenu(std::string_view a_view, Readiness a_readiness, bool a_stateBarrier,
			std::uint64_t a_tick, std::optional<Clock::time_point> a_requestedAt = std::nullopt,
			Clock::time_point a_now = Clock::now());
		// HUDs always pass through the load gate, including startup and host recovery.
		void QueueHud(std::string_view a_view, std::uint64_t a_readyTick);
		std::vector<std::string> TakeReady(std::uint64_t a_tick, bool a_hostReady, bool a_menusAllowed,
			const std::function<Readiness(std::string_view)>& a_readiness);

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
		std::optional<std::string> _menu;
		std::uint64_t _menuReadyTick{ 0 };
		std::unordered_map<std::string, std::uint64_t> _huds;
		std::optional<ColdOpenTiming> _timing;
	};
}
