#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace OSFUI
{
	// Main-thread player HUD startup overrides persist separately from shipped manifests and disabled mods.
	class ViewPolicyStore
	{
	public:
		static constexpr std::int64_t kFormatVersion = 1;

		// Missing files use defaults; malformed files are quarantined before starting empty.
		void Load(std::filesystem::path a_path);

		// Player override wins over the manifest's HUD startup default.
		[[nodiscard]] bool HudAutoStart(std::string_view a_viewId, bool a_manifestDefault) const;
		[[nodiscard]] bool HasHudOverride(std::string_view a_viewId) const;

		// Roll back in-memory state when atomic persistence fails.
		[[nodiscard]] bool SetHudAutoStart(std::string_view a_viewId, bool a_enabled);

	private:
		[[nodiscard]] bool Persist() const;

		std::filesystem::path                 _path;
		std::unordered_map<std::string, bool> _hudOverrides;
	};
}
