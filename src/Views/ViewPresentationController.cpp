#include "Views/ViewPresentationController.h"

#include <algorithm>

namespace OSFUI
{
	void ViewPresentationController::AddInstantiated(const InstantiatedView& a_view)
	{
		_instantiated[a_view.id] = a_view;
	}

	bool ViewPresentationController::RemoveInstantiated(std::string_view a_id)
	{
		const bool changed = Close(a_id);
		_instantiated.erase(std::string(a_id));
		return changed;
	}

	const ViewPresentationController::InstantiatedView* ViewPresentationController::FindInstantiated(std::string_view a_id) const
	{
		const auto it = _instantiated.find(std::string(a_id));
		return it == _instantiated.end() ? nullptr : &it->second;
	}

	bool ViewPresentationController::IsOpen(std::string_view a_id) const
	{
		const std::string id(a_id);
		return _hudShown.contains(id) || (_activeMenu && *_activeMenu == id);
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
			return _hudShown.insert(id).second;  // false if already shown
		}

		if (_activeMenu && *_activeMenu == id) {
			return false;
		}
		_activeMenu = id;
		return true;
	}

	bool ViewPresentationController::Close(std::string_view a_id)
	{
		const std::string id(a_id);
		if (_hudShown.erase(id) > 0) {
			return true;
		}
		if (_activeMenu && *_activeMenu == id) {
			_activeMenu.reset();
			return true;
		}
		return false;
	}

	bool ViewPresentationController::CloseActiveMenu()
	{
		if (!_activeMenu) {
			return false;
		}
		_activeMenu.reset();
		return true;
	}

	void ViewPresentationController::CloseAll()
	{
		// Clear menus and HUDs so transitions cannot leave the overlay visible.
		_activeMenu.reset();
		_hudShown.clear();
	}

	bool ViewPresentationController::DesiredVisible() const
	{
		return !_hudShown.empty() || _activeMenu.has_value();
	}

	bool ViewPresentationController::DesiredCapture() const
	{
		if (!_activeMenu) {
			return false;
		}
		const auto* view = FindInstantiated(*_activeMenu);
		return view && view->capturesInput;
	}

	bool ViewPresentationController::DesiredPause() const
	{
		if (!_activeMenu) {
			return false;
		}
		const auto* view = FindInstantiated(*_activeMenu);
		return view && view->pausesGame;
	}

	std::optional<std::string> ViewPresentationController::ActiveMenu() const
	{
		return _activeMenu;
	}

	std::vector<ViewPresentationController::Layer> ViewPresentationController::DesiredLayers() const
	{
		std::vector<Layer> layers;
		layers.reserve(_instantiated.size());
		for (const auto& [id, view] : _instantiated) {
			Layer layer;
			layer.id = id;
			if (view.kind == ViewKind::Hud) {
				layer.hidden = !_hudShown.contains(id);
				layer.z = std::clamp(view.order, 0, 999);
			} else if (_activeMenu && *_activeMenu == id) {
				layer.hidden = false;
				layer.z = 1000;
			} else {
				layer.hidden = true;
				layer.z = 1000;  // menu band; hidden, so exact value is immaterial
			}
			layers.push_back(std::move(layer));
		}
		return layers;
	}
}
