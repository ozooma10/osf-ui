#pragma once

#include "api/SettingsMirror.h"

#include <nlohmann/json.hpp>

namespace OSFUI::API
{
	// Redeclares sdk/OSFUI_API.h's SettingChangedFn (an identical alias is a
	// legal redeclaration) so this class compiles host-side without the sdk
	// header's REX/W32 dependency. BridgeApi.cpp sees both headers, so any
	// drift is a compile error there.
	using SettingChangedFn = void (*)(const char* a_modId,
	                                  const char* a_key,
	                                  const char* a_valueJson,
	                                  void*       a_user) noexcept;

	// SubscribeSettings bookkeeping (mcm-design.md §8.2), factored out of
	// BridgeApi so the host test suite can exercise it without MessageBridge's
	// game dependencies — same split as SettingsMirror. Subscribe/Unsubscribe
	// are any-thread; OnChanged and Pump run on the MAIN thread. Consumer
	// callbacks are ONLY ever invoked from Pump, never with the lock held (a
	// callback may re-enter Subscribe/Unsubscribe).
	class SettingsSubscriptions
	{
	public:
		// ANY thread. Registers a per-mod subscription; the replay of the
		// mod's current values fires on the next Pump. Subscribing to a mod
		// not (yet) in the mirror is legal — the replay is then empty, and
		// values arrive through the store's per-mod replay when it registers.
		// Returns a nonzero token, or 0 on null/empty a_modId or null a_fn.
		std::uint32_t Subscribe(const char* a_modId, SettingChangedFn a_fn, void* a_user);
		// ANY thread. From the main thread (including inside a callback) no
		// further callbacks fire for the token once this returns.
		void Unsubscribe(std::uint32_t a_token);

		// MAIN thread: the store change feed — every committed value, wired in
		// Runtime::BuildModules next to the mirror update (mirror FIRST, so a
		// replay snapshot never lags the queued event). Queued for the next
		// Pump; dropped when the mod has no subscriber (a subscriber added
		// concurrently is covered by its own pending replay).
		void OnChanged(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value);

		// MAIN thread: dispatch pending replays (snapshotted from a_mirror),
		// then queued change events — so a fresh subscriber sees "current
		// values, then subsequent changes". A value committed between
		// Subscribe and this Pump can arrive twice (replay + queued event),
		// identical both times — benign, callbacks must be idempotent per
		// (mod, key, value) anyway.
		void Pump(const SettingsMirror& a_mirror);

	private:
		struct Subscription
		{
			std::string      modId;
			SettingChangedFn fn{ nullptr };
			void*            user{ nullptr };
			bool             needsReplay{ true };
		};
		struct Event
		{
			std::string modId;
			std::string key;
			std::string valueJson;
		};

		mutable std::mutex                              _mutex;
		std::unordered_map<std::uint32_t, Subscription> _subs;
		std::vector<Event>                              _events;    // committed values awaiting Pump
		std::uint32_t                                   _nextToken{ 1 };
	};
}
