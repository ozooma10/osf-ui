#pragma once

#include <condition_variable>
#include <thread>

namespace OSFUI::API
{
	// Redeclared here so native tests avoid SDK dependencies; BridgeApi.cpp catches drift.
	using HotkeyFn = void (*)(const char* a_modId,
	                          const char* a_key,
	                          void*       a_user) noexcept;

	// Subscribe is any-thread; main-thread Pump invokes callbacks unlocked and never replays hotkeys.
	class HotkeySubscriptions
	{
	public:
		// Any thread; returns 0 for invalid inputs and permits bindings that do not exist yet.
		std::uint32_t Subscribe(const char* a_modId, const char* a_key, HotkeyFn a_fn, void* a_user);
		// Any thread; completion guarantees the callback stopped, and self-unsubscribe is safe.
		void Unsubscribe(std::uint32_t a_token);

		// Main thread; queues a fire only when a subscriber currently matches.
		void OnFired(std::string_view a_modId, std::string_view a_key);

		// Main thread; delivers FIFO while rechecking token liveness before each unlocked call.
		void Pump();

	private:
		struct Subscription
		{
			std::string modId;
			std::string key;
			HotkeyFn    fn{ nullptr };
			void*       user{ nullptr };
		};
		struct Event
		{
			std::string modId;
			std::string key;
		};

		mutable std::mutex                              _mutex;
		std::condition_variable                         _invokeCv;
		std::uint32_t                                   _invokingToken{ 0 };
		std::thread::id                                 _invokingThread{};
		std::unordered_map<std::uint32_t, Subscription> _subs;
		std::vector<Event>                              _events;  // fires awaiting Pump
		std::uint32_t                                   _nextToken{ 1 };
	};
}
