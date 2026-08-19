#pragma once

#include <cstdint>

namespace OSFUI
{
	// Logical (authoring) view size when a manifest omits width/height.
	inline constexpr std::uint32_t kDefaultViewWidth{ 1600 };
	inline constexpr std::uint32_t kDefaultViewHeight{ 900 };

	// Menus may own input/pause; HUDs render over gameplay without capture.
	enum class ViewKind : std::uint8_t
	{
		Menu,
		Hud,
	};

	// Mirrors views/<modId>/<viewName>/manifest.json (api-freeze-plan item 1).
	struct ViewManifest
	{
		// Derive <modId>/<viewName> from the folder path and ignore legacy declared ids.
		std::string           id;
		std::string           title;
		std::string           description;  // one-line blurb for catalogs (`osfui/views` state / Mod Settings)
		// Owning mod id is the views/ folder name shared with its settings schema.
		std::string           mod;
		std::string           entry{ "index.html" };
		// Logical authoring size; the renderer scales CSS pixels to output height.
		std::uint32_t         width{ kDefaultViewWidth };
		std::uint32_t         height{ kDefaultViewHeight };
		bool                  transparent{ true };
		bool                  menuInputEligible{ true };  // menu-kind capability summary; derived from kind and serialized as compatibility field `interactive`
		std::filesystem::path rootDir;  // directory containing the manifest

		ViewKind kind{ ViewKind::Menu };  // "menu" | "hud"

		// Menu-only: while this is the active menu, route input into the page. Forced false for HUDs.
		bool capturesInput{ true };
		// Menus pause by default through SimPause; HUDs force this false.
		bool pausesGame{ true };

		// Menu: open at load. HUD: show at load.
		bool openOnStart{ false };

		// HUD-only order is clamped to 0..999 within the framework-owned band.
		std::int32_t order{ 0 };

		// Compatibility field controlling catalog visibility without disabling the view.
		bool catalogVisible{ true };  // serialized compatibility key: `hub`
		// Hide local tools from catalogs outside restart-latched developer mode.
		bool debugOnly{ false };

		// Advisory authored-against release used for update badges, never load gating.
		std::string targetVersion;

		[[nodiscard]] std::filesystem::path EntryPath() const { return rootDir / entry; }

		// Parses a_path; returns std::nullopt and logs on any validation failure.
		static std::optional<ViewManifest> Load(const std::filesystem::path& a_path);
	};
}
