#pragma once

#include <nlohmann/json.hpp>

// Replay each mod's latest state to its pages.

namespace OSFUI
{
	class RetainedStateStore
	{
	public:
		// Keys match case-insensitively; the latest spelling is kept.
		struct Entry
		{
			std::string    key;    // as the publisher spelled it
			nlohmann::json value;  // complete current value
			// Papyrus form identities are session-scoped; native state may survive game loads.
			bool           sessionScoped{ false };
		};

		// Existing keys still update after a mod reaches this capacity.
		static constexpr std::size_t kMaxKeysPerMod = 1024;

		static constexpr std::size_t kMaxMods = 256;

		// false when at capacity: the value is still delivered live but not retained.
		bool Set(std::string_view a_mod, std::string_view a_key, nlohmann::json a_value,
			bool a_sessionScoped = false);

		[[nodiscard]] const std::vector<Entry>* Find(std::string_view a_mod) const;

		void ClearSessionScoped();

	private:
		// Lower-cased mod id -> entries in insertion order.
		std::unordered_map<std::string, std::vector<Entry>> m_mods;
	};
}
