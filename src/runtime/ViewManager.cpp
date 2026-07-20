#include "runtime/ViewManager.h"

#include "runtime/Ids.h"

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

		// Two-level scan: views/<modId>/<viewName>/manifest.json. The mod folder
		// is the namespace and its name must be a valid mod id
		// ('<author>.<modname>'); dotless ids are reserved for built-ins like
		// osfui/. Top-level dirs without view subfolders are skipped naturally.
		for (const auto& modEntry : std::filesystem::directory_iterator(a_viewsDir, ec)) {
			if (!modEntry.is_directory()) {
				continue;
			}
			const auto modId = modEntry.path().filename().string();
			if (modId == "shared") {
				continue;  // the shared kit (views/shared/osfui.css), not a mod
			}
			if (std::filesystem::exists(modEntry.path() / "manifest.json", ec)) {
				REX::ERROR("ViewManager: {} uses the pre-1.0 flat layout — views live in "
						   "views/<author>.<modname>/<view>/manifest.json now; skipping",
					modEntry.path().string());
				continue;
			}
			if (!Ids::IsAcceptedModId(modId)) {
				REX::ERROR("ViewManager: skipping {} — view folders are namespaced "
						   "views/<author>.<modname>/<view>/ (lowercase [a-z0-9-] segments, exactly one "
						   "dot in the mod id); dotless names are reserved for the platform",
					modEntry.path().string());
				continue;
			}
			for (const auto& viewEntry : std::filesystem::directory_iterator(modEntry.path(), ec)) {
				if (!viewEntry.is_directory()) {
					continue;
				}
				const auto manifestPath = viewEntry.path() / "manifest.json";
				if (!std::filesystem::exists(manifestPath, ec)) {
					continue;  // asset folder, not a view
				}
				if (auto manifest = ViewManifest::Load(manifestPath)) {
					REX::INFO("ViewManager: loaded view '{}' ({}, {}x{})",
						manifest->id, manifest->title, manifest->width, manifest->height);
					_views.push_back(std::move(*manifest));
				}
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
