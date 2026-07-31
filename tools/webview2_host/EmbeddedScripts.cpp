#include "EmbeddedScripts.h"

#include "EmbeddedScripts.rc.h"

#include <Windows.h>

#include <limits>

namespace osfui::wv2
{
	namespace
	{
		[[nodiscard]] std::wstring LoadScript(const int a_resourceId)
		{
			const auto module = ::GetModuleHandleW(nullptr);
			const auto resource = ::FindResourceW(
				module, MAKEINTRESOURCEW(a_resourceId), MAKEINTRESOURCEW(10));
			if (!resource) return {};
			const auto bytes = ::SizeofResource(module, resource);
			const auto loaded = ::LoadResource(module, resource);
			const auto* data = static_cast<const char*>(::LockResource(loaded));
			if (!data || bytes == 0 || bytes > static_cast<DWORD>((std::numeric_limits<int>::max)())) {
				return {};
			}
			const auto size = static_cast<int>(bytes);
			const auto chars = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
				data, size, nullptr, 0);
			if (chars <= 0) return {};
			std::wstring result(static_cast<std::size_t>(chars), L'\0');
			if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
					data, size, result.data(), chars) != chars) {
				return {};
			}
			return result;
		}
	}

	const std::wstring& GetEmbeddedScript(const EmbeddedScript a_script)
	{
		static const auto bridge = LoadScript(IDR_OSFUI_BRIDGE_SHIM);
		static const auto stats = LoadScript(IDR_OSFUI_RENDER_STATS);
		static const auto network = LoadScript(IDR_OSFUI_NETWORK_GUARD);
		switch (a_script) {
		case EmbeddedScript::BridgeShim:   return bridge;
		case EmbeddedScript::RenderStats:  return stats;
		case EmbeddedScript::NetworkGuard: return network;
		}
		return bridge;
	}
}