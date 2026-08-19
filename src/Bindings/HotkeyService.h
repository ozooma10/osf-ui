#pragma once

#include "Input/InputTypes.h"

namespace OSFUI
{
	class SettingsStore;

	// Hotkey dispatch service. key presses routes to registered mods.
	class HotkeyService
	{
	public:
		// true while press must not fire (overlay capturing input or rebind capture armed)
		void SetSuppression(std::function<bool()> a_suppressed) { m_suppressed = std::move(a_suppressed); }

		// rebuild registry. called when registry changes
		void Rebuild(const SettingsStore& a_store);

		void OnKeyDown(ScanCode a_scan);

		using FireFn = std::function<void(const std::string& a_modId, const std::string& a_key)>;
		void Drain(const FireFn& a_fire);

	private:
		struct Binding
		{
			std::string mod;
			std::string key;
		};

		static constexpr std::size_t kMaxPendingFires = 64;

		std::function<bool()> m_suppressed;  // wired once; read on the window thread

		mutable std::mutex                                 m_mutex;
		std::unordered_map<ScanCode, std::vector<Binding>> m_bindings;
		std::vector<Binding>                               m_pending;
	};
}
