#pragma once

namespace OSFUI::API
{
	// Redeclares sdk/OSFUI_API.h's HotkeyFn (an identical alias is a legal
	// redeclaration) so this class compiles host-side without the sdk header's
	// REX/W32 dependency. BridgeApi.cpp sees both headers, so any drift is a
	// compile error there.
	using HotkeyFn = void (*)(const char* a_modId,
	                          const char* a_key,
	                          void*       a_user) noexcept;

	// SubscribeHotkey bookkeeping (mcm-design.md §9), factored out of
	// BridgeApi so the host test suite can exercise it without MessageBridge's
	// game dependencies — same split as SettingsSubscriptions. Subscribe/
	// Unsubscribe are any-thread; OnFired and Pump run on the MAIN thread.
	// Consumer callbacks are ONLY ever invoked from Pump, never with the lock
	// held (a callback may re-enter Subscribe/Unsubscribe).
	//
	// Unlike SettingsSubscriptions there is NO replay: a hotkey is an event,
	// not state — subscribing only tells you about future presses. And unlike
	// the per-mod settings feed, subscriptions here are per-(mod, key): the
	// subscription IS the delivery opt-in (every key-typed setting
	// participates in dispatch; only subscribed ones reach native code).
	class HotkeySubscriptions
	{
	public:
		// ANY thread. Registers a per-(mod, key) subscription. Subscribing to
		// a not-(yet-)existing or non-key setting is legal — it just never
		// fires until such a binding dispatches. Returns a nonzero token, or 0
		// on null/empty a_modId or a_key, or null a_fn.
		std::uint32_t Subscribe(const char* a_modId, const char* a_key, HotkeyFn a_fn, void* a_user);
		// ANY thread. From the main thread (including inside a callback) no
		// further callbacks fire for the token once this returns.
		void Unsubscribe(std::uint32_t a_token);

		// MAIN thread: a dispatched hotkey (Runtime::DrainHotkeys). Queued for
		// the next Pump; dropped when no subscriber matches (mod, key).
		void OnFired(std::string_view a_modId, std::string_view a_key);

		// MAIN thread: deliver queued fires FIFO to the subscriber set as of
		// this Pump — a subscriber added from a callback mid-Pump starts with
		// the NEXT fire. Invoked unlocked, re-checking token liveness per call
		// so an Unsubscribe (including from an earlier callback this Pump)
		// stops delivery immediately.
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
		std::unordered_map<std::uint32_t, Subscription> _subs;
		std::vector<Event>                              _events;  // fires awaiting Pump
		std::uint32_t                                   _nextToken{ 1 };
	};
}
