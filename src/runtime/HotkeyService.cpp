#include "runtime/HotkeyService.h"

#include "core/Log.h"
#include "runtime/SettingsStore.h"

namespace OSFUI
{
	void HotkeyService::Rebuild(const SettingsStore& a_store)
	{
		// Build outside the lock (ResolveKeyName may log), then swap in.
		std::unordered_map<KeyCode, std::vector<Binding>> bindings;
		std::size_t count = 0;
		for (const auto& setting : a_store.KeySettings()) {
			const auto vk = ResolveKeyName(setting.name);
			if (vk != kInvalidKeyCode) {
				bindings[vk].push_back({ setting.modId, setting.key });
				++count;
			}
		}
		{
			std::lock_guard lock(_mutex);
			_bindings = std::move(bindings);
		}
		if (Log::DevMode()) {
			REX::DEBUG("HotkeyService: registry rebuilt — {} binding(s)", count);
		}
	}

	void HotkeyService::OnKeyDown(KeyCode a_vk)
	{
		if (_suppressed && _suppressed()) {
			return;  // typing in a view / rebinding — never a hotkey
		}
		std::lock_guard lock(_mutex);
		const auto it = _bindings.find(a_vk);
		if (it == _bindings.end()) {
			return;
		}
		for (const auto& binding : it->second) {
			if (_pending.size() >= kMaxPendingFires) {
				return;  // main thread stalled; shed the newest, not the oldest
			}
			_pending.push_back(binding);
		}
	}

	void HotkeyService::Drain(const FireFn& a_fire)
	{
		std::vector<Binding> fires;
		{
			std::lock_guard lock(_mutex);
			fires.swap(_pending);
		}
		if (!a_fire) {
			return;
		}
		for (const auto& fire : fires) {
			a_fire(fire.mod, fire.key);
		}
	}
}
