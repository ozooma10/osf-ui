#pragma once

#include <nlohmann/json.hpp>

#include "Core/Ids.h"  // EqualsCaseInsensitiveAscii

namespace OSFUI::API
{
	// Any-thread value mirror; lookups fall back to ASCII-insensitive matching for BSFixedString casing.
	class SettingsMirror
	{
	public:
		// Main thread: store-listener feed.
		void Update(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value);
		// Replace from SettingsStore::Data(), skipping malformed entries.
		void Rebuild(const nlohmann::json& a_data);

		// Any thread; invalid pointers, unknown names, and type mismatches return false or 0.
		[[nodiscard]] bool GetBool(const char* a_modId, const char* a_key, bool* a_out) const;
		[[nodiscard]] bool GetInt(const char* a_modId, const char* a_key, std::int64_t* a_out) const;
		[[nodiscard]] bool GetFloat(const char* a_modId, const char* a_key, double* a_out) const;
		// Returns required size including NUL and always terminates nonempty buffers.
		[[nodiscard]] std::uint32_t GetString(const char* a_modId, const char* a_key, char* a_buf, std::uint32_t a_bufLen) const;

		// Returns serialized values for subscription replay, or empty for an unknown mod.
		[[nodiscard]] std::vector<std::pair<std::string, std::string>> SnapshotMod(std::string_view a_modId) const;

		// Resolve names to authored spelling; exact case wins and an empty key resolves only the mod.
		[[nodiscard]] bool ResolveNames(std::string_view a_modId, std::string_view a_key, std::string& a_outMod, std::string& a_outKey) const;

	private:
		using Values = std::unordered_map<std::string, nlohmann::json>;

		// nullptr on unknown mod/key. Caller must hold _mutex.
		[[nodiscard]] const nlohmann::json* Find(const char* a_modId, const char* a_key) const;

		// Exact-case then ASCII-insensitive lookup; callers hold _mutex for LookupMod.
		[[nodiscard]] const std::pair<const std::string, Values>* LookupMod(std::string_view a_modId) const;
		[[nodiscard]] static const std::pair<const std::string, nlohmann::json>* LookupKey(const Values& a_values, std::string_view a_key);

		mutable std::mutex                      _mutex;
		std::unordered_map<std::string, Values> _mods;
	};
}
