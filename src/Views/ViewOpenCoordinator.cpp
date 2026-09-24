#include "Views/ViewOpenCoordinator.h"

#include <utility>

namespace OSFUI
{
	bool ViewOpenCoordinator::Contains(std::string_view a_view) const
	{
		return (_menu && *_menu == a_view) || _huds.contains(std::string(a_view));
	}

	void ViewOpenCoordinator::BeginTiming(std::string_view a_view, bool a_instantiated,
		std::optional<Clock::time_point> a_requestedAt, Clock::time_point a_now)
	{
		if (_timing && _timing->view == a_view) return;
		const auto requestedAt = a_requestedAt && *a_requestedAt != Clock::time_point{} && *a_requestedAt <= a_now ?
			*a_requestedAt : a_now;
		_timing = ColdOpenTiming{ .view = std::string(a_view), .requestedAt = requestedAt };
		if (a_instantiated) _timing->instantiatedAt = a_now;
	}

	void ViewOpenCoordinator::OnInstantiated(std::string_view a_view, Clock::time_point a_now)
	{
		if (_timing && _timing->view == a_view) _timing->instantiatedAt = a_now;
	}

	void ViewOpenCoordinator::OnLoad(std::string_view a_view, bool a_failed, Clock::time_point a_now)
	{
		if (a_failed) {
			CancelTiming(a_view);
			// A failed menu must not appear much later after recovery. HUD intent
			// survives failed loads until an explicit close or view teardown.
			if (_menu && *_menu == a_view) CancelMenu();
		} else if (_timing && _timing->view == a_view) {
			_timing->loadedAt = a_now;
		}
	}

	std::optional<ViewOpenCoordinator::Timing> ViewOpenCoordinator::FinishTiming(std::string_view a_view, Clock::time_point a_now)
	{
		if (!_timing || _timing->view != a_view) return std::nullopt;
		const auto timing = std::exchange(_timing, std::nullopt);
		if (!timing->instantiatedAt || !timing->loadedAt) return std::nullopt;
		const auto ms = [](Clock::time_point a_begin, Clock::time_point a_end) {
			return std::chrono::duration_cast<std::chrono::milliseconds>(a_end - a_begin).count();
		};
		return Timing{ timing->view, ms(timing->requestedAt, a_now),
			ms(timing->requestedAt, *timing->instantiatedAt),
			ms(*timing->instantiatedAt, *timing->loadedAt), ms(*timing->loadedAt, a_now) };
	}

	void ViewOpenCoordinator::CancelTiming(std::string_view a_view)
	{
		if (_timing && _timing->view == a_view) _timing.reset();
	}

	bool ViewOpenCoordinator::QueueMenu(std::string_view a_view, Readiness a_readiness, bool a_stateBarrier,
		std::uint64_t a_tick, std::optional<Clock::time_point> a_requestedAt, Clock::time_point a_now)
	{
		if (Contains(a_view)) return false;
		CancelMenu();
		if (!a_stateBarrier && a_readiness == Readiness::Ready) return true;
		BeginTiming(a_view, true, a_requestedAt, a_now);
		_menu = a_view;
		_menuReadyTick = a_tick + (a_stateBarrier ? 1 : 0);
		return false;
	}

	void ViewOpenCoordinator::QueueHud(std::string_view a_view, std::uint64_t a_readyTick)
	{
		_huds.try_emplace(std::string(a_view), a_readyTick);
	}

	std::vector<std::string> ViewOpenCoordinator::TakeReady(std::uint64_t a_tick, bool a_hostReady, bool a_menusAllowed,
		const std::function<Readiness(std::string_view)>& a_readiness)
	{
		std::vector<std::string> ready;
		for (auto it = _huds.begin(); it != _huds.end();) {
			if (it->second > a_tick) { ++it; continue; }
			const auto state = a_readiness(it->first);
			if (state == Readiness::Missing) {
				it = _huds.erase(it);
			} else if (a_hostReady && state == Readiness::Ready) {
				ready.push_back(it->first);
				it = _huds.erase(it);
			} else {
				++it;
			}
		}
		if (!_menu || !a_hostReady || !a_menusAllowed) return ready;
		const auto state = a_readiness(*_menu);
		if (state == Readiness::Missing || state == Readiness::InputUnavailable) {
			CancelMenu();
		} else if (a_tick >= _menuReadyTick && state == Readiness::Ready) {
			ready.push_back(std::move(*_menu));
			_menu.reset(); // timing remains until the first presentable frame
		}
		return ready;
	}

	bool ViewOpenCoordinator::CancelMenu()
	{
		if (!_menu) return false;
		CancelTiming(*_menu);
		_menu.reset();
		return true;
	}

	bool ViewOpenCoordinator::Cancel(std::string_view a_view)
	{
		CancelTiming(a_view);
		const bool hud = _huds.erase(std::string(a_view)) != 0;
		const bool menu = _menu && *_menu == a_view && CancelMenu();
		return menu || hud;
	}

	void ViewOpenCoordinator::SuspendMenus()
	{
		_menu.reset();
		_timing.reset();
	}

	void ViewOpenCoordinator::Clear()
	{
		_menu.reset();
		_huds.clear();
		_timing.reset();
	}
}
