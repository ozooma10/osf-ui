#pragma once

#include <nlohmann/json.hpp>

#include "runtime/Ids.h"  // EqualsCaseInsensitiveAscii (the Papyrus casing rationale below)

namespace OSFUI::API
{
	// Any-thread settings VALUE MIRROR (mcm-design.md §8.2): the C ABI's typed
	// getters — and the Papyrus natives — read here, never SettingsStore
	// (the store is main-thread-only). Fed on the MAIN thread by a store change
	// listener: every committed value, including the startup NotifyAll replay
	// and the per-mod replay after an incremental RegisterSchema. Registry
	// SHAPE changes (RegisterSchema / RemoveMod) rebuild the whole mirror from
	// the store document so dropped mods stop resolving. Wired in
	// Runtime::BuildModules; owned by BridgeApi.
	//
	// The mirror carries VALUES only, no schema — a "type mismatch" is judged
	// by the stored value's JSON shape: GetBool wants a bool, GetInt an
	// integer number, GetFloat any number, GetString a string (covers
	// string/enum/key-typed settings).
	//
	// Lookups are exact-case first, ASCII case-insensitive on a miss. The
	// fallback exists for Papyrus (proven in-game 2026-07-17): script strings
	// intern as BSFixedString, which hands back the FIRST-seen casing
	// process-wide — a script literally writing "enabled" can arrive as
	// "Enabled" because an unrelated script interned that spelling earlier.
	// Exact-case matching is therefore unimplementable from Papyrus. For the
	// C ABI the fallback is purely additive tolerance.
	class SettingsMirror
	{
	public:
		// --- MAIN thread: store-listener feed ---
		void Update(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value);
		// Replace the whole mirror from SettingsStore::Data()'s
		// { "mods": [ { "id", "values" }, ... ] } document (malformed entries
		// are skipped, never thrown on).
		void Rebuild(const nlohmann::json& a_data);

		// --- ANY thread: the ABI getter surface. false / 0 on unknown mod/key
		// or value-shape mismatch; null a_modId/a_key/a_out tolerated. ---
		[[nodiscard]] bool GetBool(const char* a_modId, const char* a_key, bool* a_out) const;
		[[nodiscard]] bool GetInt(const char* a_modId, const char* a_key, std::int64_t* a_out) const;
		[[nodiscard]] bool GetFloat(const char* a_modId, const char* a_key, double* a_out) const;
		// Returns the required length INCLUDING the NUL (0 on unknown/mismatch);
		// copies min(a_bufLen) bytes, always NUL-terminated when a_bufLen > 0.
		[[nodiscard]] std::uint32_t GetString(const char* a_modId, const char* a_key, char* a_buf, std::uint32_t a_bufLen) const;

		// One mod's current values as (key, serialized JSON text) pairs — the
		// SubscribeSettings replay source (SettingsSubscriptions::Pump). Empty
		// for an unknown mod.
		[[nodiscard]] std::vector<std::pair<std::string, std::string>> SnapshotMod(std::string_view a_modId) const;

		// Resolve arbitrarily-cased (mod, key) to the AUTHORED spelling — what
		// PapyrusApi's queued Set/Reset ops pass the case-exact SettingsStore
		// (same BSFixedString rationale as the getter fallback). Exact match
		// wins over a case-variant sibling. a_key empty resolves the mod only
		// (whole-mod Reset; a_outKey is cleared). False when nothing matches.
		// ANY thread.
		[[nodiscard]] bool ResolveNames(std::string_view a_modId, std::string_view a_key, std::string& a_outMod, std::string& a_outKey) const;

	private:
		using Values = std::unordered_map<std::string, nlohmann::json>;

		// nullptr on unknown mod/key. Caller must hold _mutex.
		[[nodiscard]] const nlohmann::json* Find(const char* a_modId, const char* a_key) const;

		mutable std::mutex                      _mutex;
		std::unordered_map<std::string, Values> _mods;
	};
}
