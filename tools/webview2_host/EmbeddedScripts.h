#pragma once

#include <string>

namespace osfui::webview2
{
	enum class EmbeddedScript
	{
		BridgeShim,
		LegacyBridgeShim,
		NetworkGuard,
		FormControls,
		PerformanceOverlay,
	};

	[[nodiscard]] const std::wstring& GetEmbeddedScript(EmbeddedScript a_script);
}
