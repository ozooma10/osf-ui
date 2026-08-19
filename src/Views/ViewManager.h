#pragma once

#include "Views/ViewManifest.h"

namespace OSFUI
{
	// Own manifests discovered at the frozen views/<modId>/<viewName>/manifest.json path.
	class ViewManager
	{
	public:
		// Scan two levels; reject unsafe mod ids while logging other discovery failures.
		void DiscoverAll(const std::filesystem::path& a_viewsDir);

		// a_id is the qualified "<modId>/<viewName>" id.
		[[nodiscard]] const ViewManifest* Find(std::string_view a_id) const;
		[[nodiscard]] const std::vector<ViewManifest>& All() const { return _views; }

	private:
		std::vector<ViewManifest> _views;
	};
}
