#include "api/SettingsSubscriptions.h"

#include <iterator>  // make_move_iterator — not in the pch umbrella

namespace OSFUI::API
{
	std::uint32_t SettingsSubscriptions::Subscribe(const char* a_modId, SettingChangedFn a_fn, void* a_user)
	{
		if (!a_modId || !a_modId[0] || !a_fn) {
			return 0;
		}
		std::lock_guard lock(_mutex);
		// 0 is the "failed" sentinel; on the (theoretical) 4-billionth
		// subscribe, skip it and any token still live.
		while (_nextToken == 0 || _subs.contains(_nextToken)) {
			++_nextToken;
		}
		const std::uint32_t token = _nextToken++;
		_subs.emplace(token, Subscription{ std::string(a_modId), a_fn, a_user, /*needsReplay*/ true });
		return token;
	}

	void SettingsSubscriptions::Unsubscribe(std::uint32_t a_token)
	{
		std::lock_guard lock(_mutex);
		_subs.erase(a_token);
	}

	void SettingsSubscriptions::OnChanged(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value)
	{
		// Drop early when nobody listens so an unsubscribed world never
		// accumulates queue entries. Serialization happens outside the lock.
		{
			std::lock_guard lock(_mutex);
			const bool anySubscriber = std::any_of(_subs.begin(), _subs.end(),
				[&](const auto& a_entry) { return a_entry.second.modId == a_modId; });
			if (!anySubscriber) {
				return;
			}
		}
		std::string dumped = a_value.dump();
		std::lock_guard lock(_mutex);
		_events.push_back({ std::string(a_modId), std::string(a_key), std::move(dumped) });
	}

	void SettingsSubscriptions::Pump(const SettingsMirror& a_mirror)
	{
		struct Call
		{
			std::uint32_t    token;
			SettingChangedFn fn;
			void*            user;
			std::string      modId;
			std::string      key;
			std::string      valueJson;
		};
		struct Replay
		{
			std::uint32_t    token;
			SettingChangedFn fn;
			void*            user;
			std::string      modId;
		};

		// Resolve all work under one lock: replays consume their one-shot flag
		// even when the mod is unknown (late registration replays through the
		// store change feed instead, never the snapshot); events resolve to the
		// subscriber set as of this Pump.
		std::vector<Replay> replays;
		std::vector<Call>   eventCalls;
		{
			std::lock_guard lock(_mutex);
			for (auto& [token, sub] : _subs) {
				if (sub.needsReplay) {
					sub.needsReplay = false;
					replays.push_back({ token, sub.fn, sub.user, sub.modId });
				}
			}
			for (auto& ev : _events) {
				for (const auto& [token, sub] : _subs) {
					if (sub.modId == ev.modId) {
						eventCalls.push_back({ token, sub.fn, sub.user, ev.modId, ev.key, ev.valueJson });
					}
				}
			}
			_events.clear();
		}

		// Replays first (see header), expanded unlocked — the mirror has its
		// own mutex and dumps each value in SnapshotMod.
		std::vector<Call> calls;
		for (auto& r : replays) {
			for (auto& [key, valueJson] : a_mirror.SnapshotMod(r.modId)) {
				calls.push_back({ r.token, r.fn, r.user, r.modId, std::move(key), std::move(valueJson) });
			}
		}
		calls.insert(calls.end(),
			std::make_move_iterator(eventCalls.begin()), std::make_move_iterator(eventCalls.end()));

		// Invoke unlocked, re-checking liveness per call so an Unsubscribe —
		// including one issued by an earlier callback this Pump — stops
		// delivery immediately.
		for (const auto& c : calls) {
			{
				std::lock_guard lock(_mutex);
				if (!_subs.contains(c.token)) {
					continue;
				}
			}
			c.fn(c.modId.c_str(), c.key.c_str(), c.valueJson.c_str(), c.user);
		}
	}
}
