#pragma once

// Thin conveniences over CommonLibSF's REX logging (initialized by SFSE::Init).
// Use REX::INFO/WARN/ERROR/DEBUG directly for normal logging; the helpers here
// exist for per-frame code that must warn exactly once instead of spamming.

namespace OSFUI::Log
{
	// Logs a warning the first time a given call site passes `a_flag`; no-op after.
	// Usage:
	//   static std::once_flag once;
	//   Log::WarnOnce(once, "ControlLayer: BSInputEnableManager not ready");
	void WarnOnce(std::once_flag& a_flag, std::string_view a_message);

	// True when config devMode is enabled; gates chatty per-call logging.
	[[nodiscard]] bool DevMode();
	void               SetDevMode(bool a_enabled);
}
