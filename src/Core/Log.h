#pragma once

namespace OSFUI::Log
{
	// Logs a warning the first time a given call site passes `a_flag`; no-op after.
	// Usage:
	//   static std::once_flag once;
	//   Log::WarnOnce(once, "ControlLayer: BSInputEnableManager not ready");
	void WarnOnce(std::once_flag& a_flag, std::string_view a_message);

	// True when developer mode is enabled; gates chatty per-call logging.
	[[nodiscard]] bool DevMode();
	// Records the effective developer-mode flag and sets the log floor: Info for normal play,
	// Debug (the full firehose) when developer mode is on. Call once, right after
	// config load.
	void SetDevMode(bool a_enabled);
}
