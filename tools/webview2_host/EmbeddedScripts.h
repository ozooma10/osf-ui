#pragma once

#include <string>

namespace osfui::wv2
{
	enum class EmbeddedScript
	{
		BridgeShim,
		RenderStats,
		NetworkGuard,
	};

	[[nodiscard]] const std::wstring& GetEmbeddedScript(EmbeddedScript a_script);
}