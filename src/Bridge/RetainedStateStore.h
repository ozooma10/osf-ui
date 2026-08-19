#pragma once

#include <nlohmann/json.hpp>

// Main-thread, latest-wins state replayed to each document owned by the publishing mod.

namespace OSFUI
{
	class RetainedStateStore
	{
	public:
		// Match keys case-insensitively but preserve the publisher's latest spelling.
		struct Entry
		{
			std::string    key;    // as the publisher spelled it
			nlohmann::json value;  // complete current value
			// Papyrus form identities are session-scoped; native state may survive game loads.
			bool           sessionScoped{ false };
		};

		// Existing keys still update after a mod reaches this capacity.
		static constexpr std::size_t kMaxKeysPerMod = 64;

		// Bound caller-supplied mod buckets as well as keys within each bucket.
		static constexpr std::size_t kMaxMods = 256;

		// Returns false when a new value is delivered live but cannot be retained at capacity.
		bool Set(std::string_view a_mod, std::string_view a_key, nlohmann::json a_value,
			bool a_sessionScoped = false);

		// Returns one mod's values in deterministic insertion order.
		[[nodiscard]] const std::vector<Entry>* Find(std::string_view a_mod) const;

		// Drop one mod's state (its plugin unloaded / its schema went away).
		void RemoveMod(std::string_view a_mod);

		// Drop session-scoped entries while retaining native state.
		void ClearSessionScoped();

		// Drop everything (shutdown / renderer teardown).
		void Clear();

		[[nodiscard]] std::size_t ModCount() const { return _mods.size(); }

	private:
		// Canonical lowercase mod id -> bounded entries in insertion order.
		std::unordered_map<std::string, std::vector<Entry>> _mods;
	};
}
