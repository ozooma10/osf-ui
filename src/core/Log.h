#pragma once

// Thin conveniences over CommonLibSF's REX logging (initialized by SFSE::Init).
// Use REX::INFO/WARN/ERROR/DEBUG directly for normal logging; the helpers here
// exist for stub code that must warn exactly once instead of spamming per frame.

namespace PrismaSF::Log
{
	// Logs a warning the first time a given call site passes `a_flag`; no-op after.
	// Usage:
	//   static std::once_flag once;
	//   Log::WarnOnce(once, "D3D12Compositor is a stub; frames are dropped");
	void WarnOnce(std::once_flag& a_flag, std::string_view a_message);

	// True when config devMode is enabled; gates chatty per-call logging.
	[[nodiscard]] bool DevMode();
	void               SetDevMode(bool a_enabled);
}
