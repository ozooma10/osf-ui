#include "Views/ViewManager.h"

#include "Core/Ids.h"
#include "Core/Utf8Path.h"

#include <algorithm>

namespace OSFUI
{
	void ViewManager::DiscoverAll(const std::filesystem::path& a_viewsDir)
	{
		m_views.clear();

		std::error_code ec;
		if (!std::filesystem::is_directory(a_viewsDir, ec)) {
			REX::WARN("ViewManager: views dir {} does not exist; no views available", Utf8Path(a_viewsDir));
			return;
		}

		// Scan views/<modId>/<viewName>/manifest.json; only child manifests define views.
		std::filesystem::directory_iterator modIt(
			a_viewsDir, std::filesystem::directory_options::skip_permission_denied, ec);
		const std::filesystem::directory_iterator end;
		for (; modIt != end; modIt.increment(ec)) {
			const auto modEntry = *modIt;
			std::error_code entryEc;
			if (!modEntry.is_directory(entryEc)) {
				continue;
			}
			const auto modId = Utf8Path(modEntry.path().filename());
			if (!Ids::IsAcceptedModId(modId)) {
				REX::ERROR("ViewManager: skipping {} — mod folder name must use URL-safe ASCII characters (A-Z, a-z, 0-9, '.', '_', '~', '-') and cannot be a reserved name",
					Utf8Path(modEntry.path()));
				continue;
			}
			if (std::filesystem::exists(modEntry.path() / "manifest.json", entryEc)) {
				REX::ERROR("ViewManager: {} uses the pre-1.0 flat layout — views live in "
						   "views/<modId>/<view>/manifest.json now; skipping",
					Utf8Path(modEntry.path()));
				continue;
			}
			std::error_code viewEc;
			std::filesystem::directory_iterator viewIt(
				modEntry.path(), std::filesystem::directory_options::skip_permission_denied, viewEc);
			for (; viewIt != end; viewIt.increment(viewEc)) {
				const auto viewEntry = *viewIt;
				std::error_code fileEc;
				if (!viewEntry.is_directory(fileEc)) {
					continue;
				}
				const auto manifestPath = viewEntry.path() / "manifest.json";
				if (!std::filesystem::exists(manifestPath, fileEc)) {
					continue;  // asset folder, not a view
				}
				if (auto manifest = ViewManifest::Load(manifestPath)) {
					REX::INFO("ViewManager: discovered view '{}' ({}, {}x{})",
						manifest->id, manifest->title, manifest->width, manifest->height);
					m_views.push_back(std::move(*manifest));
				}
			}
		}
		// Sort qualified ids so creation, catalogs, and equal-order z ties are deterministic.
		std::ranges::sort(m_views, {}, &ViewManifest::id);
		REX::INFO("ViewManager: {} view(s) discovered under {}", m_views.size(), Utf8Path(a_viewsDir));
	}

	const ViewManifest* ViewManager::Find(std::string_view a_id) const
	{
		const auto it = std::ranges::find_if(m_views,
			[&](const auto& v) { return Ids::EqualsCaseInsensitiveAscii(v.id, a_id); });
		return it != m_views.end() ? &*it : nullptr;
	}
}
