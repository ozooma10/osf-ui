#include "Views/ViewOpenCoordinator.h"

#include <utility>

namespace OSFUI
{
	bool ViewOpenCoordinator::Contains(std::string_view a_view) const
	{
		return (m_menu && m_menu->view == a_view) || m_huds.contains(std::string(a_view));
	}

	std::optional<ViewOpenCoordinator::MenuOpen> ViewOpenCoordinator::TakeMenu()
	{
		return std::exchange(m_menu, std::nullopt);
	}

	void ViewOpenCoordinator::QueueMenu(std::string_view a_view, double a_deadline, std::uint64_t a_requestId)
	{
		m_menu = MenuOpen{ std::string(a_view), a_deadline, a_requestId };
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
		if (m_menu && m_menu->phase == Phase::Loading && a_readiness(m_menu->view) == Readiness::Ready) {
			m_menu->phase = Phase::Rendering;
			ready.push_back(m_menu->view);
		}
		return ready;
	}

	bool ViewOpenCoordinator::CancelHud(std::string_view a_view)
	{
		return m_huds.erase(std::string(a_view)) != 0;
	}

	void ViewOpenCoordinator::ClearHuds()
	{
		m_huds.clear();
	}
}
