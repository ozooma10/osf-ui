#pragma once

#include "input/InputTypes.h"

namespace OSFUI
{
	class SettingsStore;

	// Central hotkey dispatch (mcm-design.md §9): every `type:"key"` setting of
	// every registered mod is a live binding; a gameplay key press routes to the
	// setting's owner over the C ABI (SubscribeHotkey) and/or as a `ui.hotkey`
	// web push. Centralized because only OSF UI has the policy context — a mod's
	// own raw hook can't tell that the press landed in an overlay text field or
	// mid-rebind, so per-mod hooks double-fire; web-only and Papyrus mods have no
	// hook at all.
	//
	// Registry + queue only: Runtime feeds it (OnHostKey on the window thread,
	// Rebuild/Drain on the main thread) and fans drained fires out to the
	// delivery channels. Dispatch never consumes the key. The "during gameplay"
	// half is enforced by Runtime at drain time (DrainHotkeys +
	// MenuMode::AnyGameMenuOpen): presses made with a game menu open are dropped.
	class HotkeyService
	{
	public:
		// Window-thread context gate, wired once at composition (before the input
		// hook goes live): true while a press must not fire — the overlay is
		// capturing input, or a rebind capture is armed and the press is the new
		// binding.
		void SetSuppression(std::function<bool()> a_suppressed) { _suppressed = std::move(a_suppressed); }

		// Main thread: rebuild the vk -> [(mod, key)] registry from every
		// key-typed setting's current value (SettingsStore::KeySettings via
		// ResolveKeyName). Called at composition, on any key-typed commit, and on
		// registry shape change. An unresolvable name doesn't bind (ResolveKeyName
		// warns).
		void Rebuild(const SettingsStore& a_store);

		// Window thread (Runtime::OnHostKey): key-down edge only — the WndProc
		// hook filters repeats. Queues one fire per binding of a_vk unless the
		// suppression gate holds; Drain delivers on the main thread.
		void OnKeyDown(KeyCode a_vk);

		// Main thread (Runtime::Tick): deliver queued fires FIFO. The queue is
		// drained before invoking, so a_fire may re-enter the store or trigger a
		// Rebuild.
		using FireFn = std::function<void(const std::string& a_modId, const std::string& a_key)>;
		void Drain(const FireFn& a_fire);

	private:
		struct Binding
		{
			std::string mod;
			std::string key;
		};

		// Cap on presses queued while the main thread stalls; past it we drop the
		// newest, since replaying a burst of stale presses after a hitch is worse
		// than losing them. A human can't outrun this bound.
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
