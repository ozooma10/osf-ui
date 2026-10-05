#pragma once

#include "Views/ViewManifest.h"

namespace OSFUI
{
	// Owns manifests discovered at views/<modId>/<viewName>/manifest.json.
	class ViewManager
	{
	public:
		// Rejects unsafe mod ids; logs other discovery failures.
		void DiscoverAll(const std::filesystem::path& a_viewsDir);
		void AddIfAbsent(ViewManifest a_manifest);

		// a_id is the qualified "<modId>/<viewName>" id.
		[[nodiscard]] const ViewManifest* Find(std::string_view a_id) const;
		[[nodiscard]] const std::vector<ViewManifest>& All() const { return m_views; }

	private:
		std::vector<ViewManifest> m_views;
	};
}
