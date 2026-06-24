#pragma once

#include "runtime/ViewManifest.h"

namespace PrismaSF
{
	// Discovers and owns view manifests found under <data>/views/*/manifest.json.
	class ViewManager
	{
	public:
		// Scans a_viewsDir. Missing dir or bad manifests are logged, not fatal.
		void LoadAll(const std::filesystem::path& a_viewsDir);

		[[nodiscard]] const ViewManifest* Find(std::string_view a_id) const;
		[[nodiscard]] const std::vector<ViewManifest>& All() const { return _views; }

	private:
		std::vector<ViewManifest> _views;
	};
}
