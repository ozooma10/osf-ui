#pragma once

#include "runtime/ViewManifest.h"

namespace OSFUI
{
	// Discovers and owns view manifests found under
	// <data>/views/<modId>/<viewName>/manifest.json (frozen public path).
	class ViewManager
	{
	public:
		// Scans a_viewsDir (two-level). Missing dir or bad manifests are
		// logged, not fatal; folders violating the id grammar are hard-rejected
		// with an ERROR.
		void LoadAll(const std::filesystem::path& a_viewsDir);

		// a_id is the qualified "<modId>/<viewName>" id.
		[[nodiscard]] const ViewManifest* Find(std::string_view a_id) const;
		[[nodiscard]] const std::vector<ViewManifest>& All() const { return _views; }

	private:
		std::vector<ViewManifest> _views;
	};
}
