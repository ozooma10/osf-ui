#include "runtime/ViewManager.h"

namespace OSFUI
{
	void ViewManager::LoadAll(const std::filesystem::path& a_viewsDir)
	{
		_views.clear();

		std::error_code ec;
		if (!std::filesystem::is_directory(a_viewsDir, ec)) {
			REX::WARN("ViewManager: views dir {} does not exist; no views available", a_viewsDir.string());
			return;
		}

		for (const auto& entry : std::filesystem::directory_iterator(a_viewsDir, ec)) {
			if (!entry.is_directory()) {
				continue;
			}
			const auto manifestPath = entry.path() / "manifest.json";
			if (!std::filesystem::exists(manifestPath, ec)) {
				REX::WARN("ViewManager: {} has no manifest.json; skipping", entry.path().string());
				continue;
			}
			if (auto manifest = ViewManifest::Load(manifestPath)) {
				REX::INFO("ViewManager: loaded view '{}' ({}, {}x{})",
					manifest->id, manifest->title, manifest->width, manifest->height);
				_views.push_back(std::move(*manifest));
			}
		}
		REX::INFO("ViewManager: {} view(s) loaded from {}", _views.size(), a_viewsDir.string());
	}

	const ViewManifest* ViewManager::Find(std::string_view a_id) const
	{
		const auto it = std::ranges::find_if(_views, [&](const auto& v) { return v.id == a_id; });
		return it != _views.end() ? &*it : nullptr;
	}
}
