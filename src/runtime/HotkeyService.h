#pragma once

#include "input/InputTypes.h"

namespace OSFUI
{
	class SettingsStore;

	// Central hotkey dispatch (mcm-design.md §9): every `type:"key"` setting
	// of every registered mod is a live binding — a physical key press during
	// gameplay routes to the setting's owner over the C ABI (SubscribeHotkey)
	// and/or as a `ui.hotkey` web push. Centralized because OSF UI owns both
	// the input hook AND the policy context: a mod's own raw hook can't know a
	// press happened while the user was typing in an overlay text field or
	// mid-rebind, so per-mod hooks systematically double-fire; and web-only or
	// Papyrus mods have no input hook at all — this service is the only way
	// their `key` settings ever DO anything.
	//
	// The service is pure registry + queue: Runtime feeds it (OnHostKey on the
	// window thread, Rebuild/Drain on the main thread) and fans the drained
	// fires out to the delivery channels. Dispatch never consumes the key.
	// The "during gameplay" half of the contract is enforced by Runtime at
	// drain time (DrainHotkeys + MenuMode::AnyGameMenuOpen): presses made
	// while a game menu is open are dropped, never delivered.
	class HotkeyService
	{
	public:
		// The window-thread context gate, wired once at composition (before
		// the input hook is live): returns true while a press must NOT fire —
		// the overlay captures input (the user is typing into a view) or a
		// rebind capture is armed (the press IS the new binding).
		void SetSuppression(std::function<bool()> a_suppressed) { _suppressed = std::move(a_suppressed); }

		// MAIN thread: rebuild the vk -> [(mod, key)] registry from every
		// key-typed setting's current value (SettingsStore::KeySettings via
		// ResolveKeyName). Called at composition, on any key-typed commit, and
		// on registry shape change. A name that doesn't resolve simply doesn't
		// bind (warned by ResolveKeyName).
		void Rebuild(const SettingsStore& a_store);

		// WINDOW thread (Runtime::OnHostKey): a key-DOWN edge (repeats never
		// reach here — the WndProc hook filters them). Queues one fire per
		// binding of a_vk unless the suppression gate holds; Drain delivers on
		// the main thread.
		void OnKeyDown(KeyCode a_vk);

		// MAIN thread (Runtime::Tick): deliver queued fires FIFO. The queue is
		// drained before invoking, so a_fire may safely re-enter the store /
		// trigger a Rebuild.
		using FireFn = std::function<void(const std::string& a_modId, const std::string& a_key)>;
		void Drain(const FireFn& a_fire);

	private:
		struct Binding
		{
			std::string mod;
			std::string key;
		};

		// Presses queued while the main thread stalls are dropped past this
		// (drop-NEWEST: replaying a burst of stale presses after a hitch is
		// worse than losing it). Generous — a human can't outrun it.
		static constexpr std::size_t kMaxPendingFires = 64;

		std::function<bool()> _suppressed;  // wired once; read on the window thread

		// Leaf lock: _bindings is rebuilt on the main thread and read on the
		// window thread; _pending is written on the window thread and drained
		// on the main thread. Snapshot under it, act unlocked.
		mutable std::mutex                                _mutex;
		std::unordered_map<KeyCode, std::vector<Binding>> _bindings;
		std::vector<Binding>                              _pending;
	};
}
