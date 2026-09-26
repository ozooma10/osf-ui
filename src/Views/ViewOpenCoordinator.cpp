#include "Views/ViewOpenCoordinator.h"

#include <utility>

namespace OSFUI
{
	bool ViewOpenCoordinator::Contains(std::string_view a_view) const
	{
		return (m_menu && *m_menu == a_view) || m_huds.contains(std::string(a_view));
	}

	void ViewOpenCoordinator::OnLoadFailed(std::string_view a_view)
	{
		if (m_menu && *m_menu == a_view) CancelMenu();
	}

	bool ViewOpenCoordinator::QueueMenu(std::string_view a_view, Readiness a_readiness)
	{
		if (Contains(a_view)) return false;
		CancelMenu();
		if (a_readiness == Readiness::Ready) return true;
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
			m_menu.reset();
		}
		return ready;
	}

	bool ViewOpenCoordinator::CancelMenu()
	{
		if (!m_menu) return false;
		m_menu.reset();
		return true;
	}

	bool ViewOpenCoordinator::Cancel(std::string_view a_view)
	{
		const bool hud = m_huds.erase(std::string(a_view)) != 0;
		const bool menu = m_menu && *m_menu == a_view && CancelMenu();
		return menu || hud;
	}

	void ViewOpenCoordinator::Clear()
	{
		m_menu.reset();
		m_huds.clear();
	}
}
