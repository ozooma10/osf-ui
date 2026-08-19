#pragma once

#include "API/SettingsMirror.h"

#include <condition_variable>
#include <nlohmann/json.hpp>
#include <thread>

namespace OSFUI::API
{
	// Redeclared here so native tests avoid SDK dependencies; BridgeApi.cpp catches drift.
	using SettingChangedFn = void (*)(const char* a_modId,
	                                  const char* a_key,
	                                  const char* a_valueJson,
	                                  void*       a_user) noexcept;

	// Subscribe is any-thread; main-thread Pump invokes callbacks without holding the lock.
	class SettingsSubscriptions
	{
	public:
		// Any thread; returns 0 for invalid inputs and schedules one current-value replay.
		std::uint32_t Subscribe(const char* a_modId, SettingChangedFn a_fn, void* a_user);
		// Any thread; completion guarantees the callback stopped, and self-unsubscribe is safe.
		void Unsubscribe(std::uint32_t a_token);

		// Main thread; queues committed values only while the mod has a subscriber.
		void OnChanged(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value);

		// Main thread; replays precede changes, and callers must tolerate a duplicate current value.
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
		std::condition_variable                         _invokeCv;
		std::uint32_t                                   _invokingToken{ 0 };
		std::thread::id                                 _invokingThread{};
		std::unordered_map<std::uint32_t, Subscription> _subs;
		std::vector<Event>                              _events;    // committed values awaiting Pump
		std::uint32_t                                   _nextToken{ 1 };
	};
}
