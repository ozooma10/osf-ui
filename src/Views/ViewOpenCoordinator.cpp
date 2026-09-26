#include "Views/ViewOpenCoordinator.h"

#include <utility>

namespace OSFUI
{
	bool ViewOpenCoordinator::Contains(std::string_view a_view) const
	{
		return (m_menu && *m_menu == a_view) || m_huds.contains(std::string(a_view));
	}

	void ViewOpenCoordinator::BeginTiming(std::string_view a_view, bool a_instantiated,
		std::optional<Clock::time_point> a_requestedAt, Clock::time_point a_now)
	{
		if (m_timing && m_timing->view == a_view) return;
		const auto requestedAt = a_requestedAt && *a_requestedAt != Clock::time_point{} && *a_requestedAt <= a_now ?
			*a_requestedAt : a_now;
		m_timing = ColdOpenTiming{ .view = std::string(a_view), .requestedAt = requestedAt };
		if (a_instantiated) m_timing->instantiatedAt = a_now;
	}

	void ViewOpenCoordinator::OnInstantiated(std::string_view a_view, Clock::time_point a_now)
	{
		if (m_timing && m_timing->view == a_view) m_timing->instantiatedAt = a_now;
	}

	void ViewOpenCoordinator::OnLoad(std::string_view a_view, bool a_failed, Clock::time_point a_now)
	{
		if (a_failed) {
			CancelTiming(a_view);
			// A failed menu must not appear much later after recovery. HUD intent
			// survives failed loads until an explicit close or view teardown.
			if (m_menu && *m_menu == a_view) CancelMenu();
		} else if (m_timing && m_timing->view == a_view) {
			m_timing->loadedAt = a_now;
		}
	}

	std::optional<ViewOpenCoordinator::Timing> ViewOpenCoordinator::FinishTiming(std::string_view a_view, Clock::time_point a_now)
	{
		if (!m_timing || m_timing->view != a_view) return std::nullopt;
		const auto timing = std::exchange(m_timing, std::nullopt);
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
		if (m_timing && m_timing->view == a_view) m_timing.reset();
	}

	bool ViewOpenCoordinator::QueueMenu(std::string_view a_view, Readiness a_readiness,
		std::optional<Clock::time_point> a_requestedAt, Clock::time_point a_now)
	{
		if (Contains(a_view)) return false;
		CancelMenu();
		if (a_readiness == Readiness::Ready) return true;
		BeginTiming(a_view, true, a_requestedAt, a_now);
		m_menu = a_view;
		return false;
	}

	void ViewOpenCoordinator::QueueHud(std::string_view a_view)
	{
		m_huds.emplace(a_view);
	}

	std::vector<std::string> ViewOpenCoordinator::TakeReady(const std::function<Readiness(std::string_view)>& a_readiness)
	{
		std::vector<std::string> ready;
		for (auto it = m_huds.begin(); it != m_huds.end();) {
			const auto state = a_readiness(*it);
			if (state == Readiness::Missing) {
				it = m_huds.erase(it);
			} else if (state == Readiness::Ready) {
				ready.push_back(*it);
				it = m_huds.erase(it);
			} else {
				++it;
			}
		}
		if (!m_menu) return ready;
		const auto state = a_readiness(*m_menu);
		if (state == Readiness::Missing || state == Readiness::InputUnavailable) {
			CancelMenu();
		} else if (state == Readiness::Ready) {
			ready.push_back(std::move(*m_menu));
			m_menu.reset(); // timing remains until the first presentable frame
		}
		return ready;
	}

	bool ViewOpenCoordinator::CancelMenu()
	{
		if (!m_menu) return false;
		CancelTiming(*m_menu);
		m_menu.reset();
		return true;
	}

	bool ViewOpenCoordinator::Cancel(std::string_view a_view)
	{
		CancelTiming(a_view);
		const bool hud = m_huds.erase(std::string(a_view)) != 0;
		const bool menu = m_menu && *m_menu == a_view && CancelMenu();
		return menu || hud;
	}

	void ViewOpenCoordinator::SuspendMenus()
	{
		m_menu.reset();
		m_timing.reset();
	}

	void ViewOpenCoordinator::Clear()
	{
		m_menu.reset();
		m_huds.clear();
		m_timing.reset();
	}
}
