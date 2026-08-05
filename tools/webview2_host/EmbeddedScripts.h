#pragma once

#include <string>

namespace osfui::wv2
{
	enum class EmbeddedScript
	{
		BridgeShim,
		NetworkGuard,
	};

	[[nodiscard]] const std::wstring& GetEmbeddedScript(EmbeddedScript a_script);
}
