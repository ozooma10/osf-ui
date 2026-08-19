#include "Bindings/HotkeyService.h"

#include "Core/Log.h"
#include "Input/KeyNames.h"
#include "Settings/SettingsStore.h"

namespace OSFUI
{
	void HotkeyService::Rebuild(const SettingsStore& a_store)
	{
		// Build outside the lock (ResolveKeyName may log), then swap in.
		std::unordered_map<ScanCode, std::vector<Binding>> bindings;
		std::size_t count = 0;
		for (const auto& setting : a_store.KeySettings()) {
			const auto scan = ResolveKeyName(setting.name);
			if (scan != kInvalidScanCode) {
				bindings[scan].push_back({ setting.modId, setting.key });
				++count;
			}
		}
		{
			std::lock_guard lock(m_mutex);
			m_bindings = std::move(bindings);
		}
		if (Log::DebugEnabled()) {
			REX::DEBUG("HotkeyService: registry rebuilt — {} binding(s)", count);
		}
	}

	void HotkeyService::OnKeyDown(ScanCode a_scan)
	{
		if (m_suppressed && m_suppressed()) {
			return;  // typing in a view / rebinding — never a hotkey
		}
		std::lock_guard lock(m_mutex);
		const auto it = m_bindings.find(a_scan);
		if (it == m_bindings.end()) {
			return;
		}
		for (const auto& binding : it->second) {
			if (m_pending.size() >= kMaxPendingFires) {
				return;  // main thread stalled; shed the newest, not the oldest
			}
			m_pending.push_back(binding);
		}
	}

	void HotkeyService::Drain(const FireFn& a_fire)
	{
		std::vector<Binding> fires;
		{
			std::lock_guard lock(m_mutex);
			fires.swap(m_pending);
		}
		if (!a_fire) {
			return;
		}
		for (const auto& fire : fires) {
			a_fire(fire.mod, fire.key);
		}
	}
}
