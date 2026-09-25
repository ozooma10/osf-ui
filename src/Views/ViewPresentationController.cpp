#include "Views/ViewPresentationController.h"

#include <algorithm>
#include <utility>

namespace OSFUI
{
	void ViewPresentationController::AddInstantiated(const InstantiatedView& a_view)
	{
		m_instantiated[a_view.id] = a_view;
		m_changed = true;
	}

	bool ViewPresentationController::RemoveInstantiated(std::string_view a_id)
	{
		const bool changed = Close(a_id);
		if (m_instantiated.erase(std::string(a_id))) m_changed = true;
		return changed;
	}

	bool ViewPresentationController::TakeChanged()
	{
		return std::exchange(m_changed, false);
	}

	const ViewPresentationController::InstantiatedView* ViewPresentationController::FindInstantiated(std::string_view a_id) const
	{
		const auto it = m_instantiated.find(std::string(a_id));
		return it == m_instantiated.end() ? nullptr : &it->second;
	}

	bool ViewPresentationController::IsOpen(std::string_view a_id) const
	{
		const std::string id(a_id);
		return m_hudShown.contains(id) || (m_activeMenu && *m_activeMenu == id);
	}

	bool ViewPresentationController::IsInstantiated(std::string_view a_id) const
	{
		return FindInstantiated(a_id) != nullptr;
	}

	bool ViewPresentationController::Open(std::string_view a_id)
	{
		const auto* view = FindInstantiated(a_id);
		if (!view) {
			return false;  // not instantiated
		}
		const std::string id(a_id);

		if (view->kind == ViewKind::Hud) {
			const bool added = m_hudShown.insert(id).second;
			m_changed = m_changed || added;
			return added;
		}

		if (m_activeMenu && *m_activeMenu == id) {
			return false;
		}
		m_activeMenu = id;
		m_changed = true;
		return true;
	}

	bool ViewPresentationController::Close(std::string_view a_id)
	{
		const std::string id(a_id);
		if (m_hudShown.erase(id) > 0) {
			m_changed = true;
			return true;
		}
		if (m_activeMenu && *m_activeMenu == id) {
			m_activeMenu.reset();
			m_changed = true;
			return true;
		}
		return false;
	}

	bool ViewPresentationController::CloseActiveMenu()
	{
		if (!m_activeMenu) {
			return false;
		}
		m_activeMenu.reset();
		m_changed = true;
		return true;
	}

	void ViewPresentationController::CloseAll()
	{
		// Clear menus and HUDs so transitions cannot leave the overlay visible.
		m_changed = m_changed || m_activeMenu.has_value() || !m_hudShown.empty();
		m_activeMenu.reset();
		m_hudShown.clear();
	}

	bool ViewPresentationController::SetSuspended(bool a_suspended)
	{
		if (m_suspended == a_suspended) return false;
		m_suspended = a_suspended;
		m_changed = true;
		if (m_suspended) m_activeMenu.reset();
		return true;
	}

	bool ViewPresentationController::DesiredVisible() const
	{
		return !m_suspended && (!m_hudShown.empty() || m_activeMenu.has_value());
	}

	bool ViewPresentationController::DesiredCapture() const
	{
		if (m_suspended || !m_activeMenu) {
			return false;
		}
		const auto* view = FindInstantiated(*m_activeMenu);
		return view && view->capturesInput;
	}

	bool ViewPresentationController::DesiredPause() const
	{
		if (m_suspended || !m_activeMenu) {
			return false;
		}
		const auto* view = FindInstantiated(*m_activeMenu);
		return view && view->pausesGame;
	}

	std::optional<std::string> ViewPresentationController::ActiveMenu() const
	{
		return m_suspended ? std::nullopt : m_activeMenu;
	}

	std::vector<ViewPresentationController::Layer> ViewPresentationController::DesiredLayers() const
	{
		std::vector<Layer> layers;
		layers.reserve(m_instantiated.size());
		for (const auto& [id, view] : m_instantiated) {
			Layer layer;
			layer.id = id;
			if (view.kind == ViewKind::Hud) {
				layer.hidden = !m_hudShown.contains(id);
				layer.z = std::clamp(view.order, 0, 999);
			} else if (m_activeMenu && *m_activeMenu == id) {
				layer.hidden = false;
				layer.z = 1000;
			} else {
				layer.hidden = true;
				layer.z = 1000;  // menu band; hidden, so exact value is immaterial
			}
			layer.hidden = layer.hidden || m_suspended;
			layers.push_back(std::move(layer));
		}
		return layers;
	}
}
