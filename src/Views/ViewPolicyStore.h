#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace OSFUI
{
	// Main-thread-only player policy for view startup: which discovered HUDs
	// start automatically. Shipped mod files stay untouched — the manifest's
	// openOnStart is only the author default, and the player's overrides live
	// beside the settings values in the plugin data dir. Overrides are retained
	// for views that are not currently installed, so temporarily disabling a mod
	// does not lose the choice.
	class ViewPolicyStore
	{
	public:
		static constexpr std::int64_t kFormatVersion = 1;

		// Missing file = empty policy (defaults rule). A malformed file is
		// quarantined to <name>.bad and the store starts empty; the fresh file
		// written by the next SetHudAutoStart replaces it.
		void Load(std::filesystem::path a_path);

		// Effective automatic-start for a HUD: the player's override when one
		// exists, else the manifest's openOnStart default.
		[[nodiscard]] bool HudAutoStart(std::string_view a_viewId, bool a_manifestDefault) const;
		[[nodiscard]] bool HasHudOverride(std::string_view a_viewId) const;

		// Records and persists an override (temp file + rename, like the settings
		// values). On a failed write the previous in-memory state is restored and
		// false is returned, so what the UI reports never drifts from what the
		// next launch will actually read.
		[[nodiscard]] bool SetHudAutoStart(std::string_view a_viewId, bool a_enabled);

	private:
		[[nodiscard]] bool Persist() const;

		std::filesystem::path                 _path;
		std::unordered_map<std::string, bool> _hudOverrides;
	};
}
