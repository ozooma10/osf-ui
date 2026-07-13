#include "api/HotkeySubscriptions.h"

namespace OSFUI::API
{
	std::uint32_t HotkeySubscriptions::Subscribe(const char* a_modId, const char* a_key, HotkeyFn a_fn, void* a_user)
	{
		if (!a_modId || !a_modId[0] || !a_key || !a_key[0] || !a_fn) {
			return 0;
		}
		std::lock_guard lock(_mutex);
		// 0 is the "failed" sentinel; on the (theoretical) 4-billionth
		// subscribe, skip it and any token still live.
		while (_nextToken == 0 || _subs.contains(_nextToken)) {
			++_nextToken;
		}
		const std::uint32_t token = _nextToken++;
		_subs.emplace(token, Subscription{ std::string(a_modId), std::string(a_key), a_fn, a_user });
		return token;
	}

	void HotkeySubscriptions::Unsubscribe(std::uint32_t a_token)
	{
		std::lock_guard lock(_mutex);
		_subs.erase(a_token);
	}

	void HotkeySubscriptions::OnFired(std::string_view a_modId, std::string_view a_key)
	{
		// Drop early when nobody listens so an unsubscribed hotkey never
		// accumulates queue entries (dispatch fires for EVERY key-typed
		// setting; most have no native consumer).
		std::lock_guard lock(_mutex);
		const bool anySubscriber = std::any_of(_subs.begin(), _subs.end(),
			[&](const auto& a_entry) { return a_entry.second.modId == a_modId && a_entry.second.key == a_key; });
		if (!anySubscriber) {
			return;
		}
		_events.push_back({ std::string(a_modId), std::string(a_key) });
	}

	void HotkeySubscriptions::Pump()
	{
		struct Call
		{
			std::uint32_t token;
			HotkeyFn      fn;
			void*         user;
			std::string   modId;
			std::string   key;
		};

		// Resolve the queued fires to the subscriber set under one lock, then
		// invoke unlocked with a per-call liveness re-check (the
		// SettingsSubscriptions::Pump discipline).
		std::vector<Call> calls;
		{
			std::lock_guard lock(_mutex);
			for (auto& ev : _events) {
				for (const auto& [token, sub] : _subs) {
					if (sub.modId == ev.modId && sub.key == ev.key) {
						calls.push_back({ token, sub.fn, sub.user, ev.modId, ev.key });
					}
				}
			}
			_events.clear();
		}

		for (const auto& c : calls) {
			{
				std::lock_guard lock(_mutex);
				if (!_subs.contains(c.token)) {
					continue;
				}
			}
			c.fn(c.modId.c_str(), c.key.c_str(), c.user);
		}
	}
}
