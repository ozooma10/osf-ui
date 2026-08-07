#pragma once

#include <nlohmann/json.hpp>

// Retained mod state: mod-backend-owned values replayed to each current and
// future document owned by the publishing mod (docs/terminology.md).
//
// This is the systemic fix for the blank-after-F5 bug class. A mod backend that
// knows when a value changes PUBLISHES it here once; the OSF UI runtime replays every
// current value to each document that greets the bridge. Nothing in a view has
// to remember to re-request anything, and nothing in a mod backend has to listen
// for a view-defined hello.
//
// State is latest-wins and complete per key — never a delta — so a replay and a
// live update are the same message. Events are the other half of the pair and
// deliberately do NOT live here: replaying a one-shot happening re-fires its
// effect.
//
// Both mod backend types share this store, which makes their state semantics
// square: Papyrus `SetView*` and the native ABI's `SetViewState` land in the
// same place and replay by the same rule. Main thread only — Papyrus natives
// and ABI callers queue their writes and Runtime drains them on the tick.

namespace OSFUI
{
	class RetainedStateStore
	{
	public:
		// Keys are matched case-insensitively: a Papyrus key arrives through
		// BSFixedString interning, which hands back the first-seen casing
		// process-wide, so the script's literal spelling does not survive.
		// The ORIGINAL casing is kept for delivery so a view sees what the
		// author wrote.
		struct Entry
		{
			std::string    key;    // as the publisher spelled it
			nlohmann::json value;  // complete current value
			// Papyrus values may hold session-scoped form identities, so they
			// must not survive a game load. A native plugin's state has no such
			// constraint — wiping its HUD config on every load would be a bug —
			// so the scope travels with the entry rather than being one policy
			// for the whole store.
			bool           sessionScoped{ false };
		};

		// Cap per mod. A mod looping on SetView* with generated keys hits this
		// instead of growing the process without bound; the value of an
		// ALREADY-retained key always updates, so a mod with a fixed key set is
		// never affected.
		static constexpr std::size_t kMaxKeysPerMod = 64;

		// Cap on distinct mods, for the same reason. The per-key cap alone
		// bounds nothing on its own: mod ids come from Papyrus and native
		// callers, so an unbounded number of buckets is an unbounded store.
		static constexpr std::size_t kMaxMods = 256;

		// Publish (or replace) one key's complete value. Returns false when the
		// mod is at capacity with a key it does not already hold — the value is
		// then delivered live but not retained, so it will not survive a reload.
		bool Set(std::string_view a_mod, std::string_view a_key, nlohmann::json a_value,
			bool a_sessionScoped = false);

		// Every retained value for one mod, in insertion order so a replay is
		// deterministic and reads like the publisher's own sequence.
		[[nodiscard]] const std::vector<Entry>* Find(std::string_view a_mod) const;

		// Drop one mod's state (its plugin unloaded / its schema went away).
		void RemoveMod(std::string_view a_mod);

		// Drop every session-scoped entry (a game load happened). Native state
		// is left alone.
		void ClearSessionScoped();

		// Drop everything (shutdown / renderer teardown).
		void Clear();

		[[nodiscard]] std::size_t ModCount() const { return _mods.size(); }

	private:
		// Canonical lowercase mod id -> that mod's entries. A vector rather than
		// a map because the sets are tiny, bounded, and always iterated whole.
		std::unordered_map<std::string, std::vector<Entry>> _mods;
	};
}
