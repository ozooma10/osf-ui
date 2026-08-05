#include "EmbeddedScripts.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <limits>

namespace osfui::wv2
{
	namespace
	{
		const unsigned char kBridgeShim[] = {
#include "bridge-shim.js.h"
		};
		const unsigned char kNetworkGuard[] = {
#include "network-guard.js.h"
		};

		template <std::size_t N>
		[[nodiscard]] std::wstring LoadScript(const unsigned char (&a_data)[N])
		{
			static_assert(N > 0 && N - 1 <= static_cast<std::size_t>((std::numeric_limits<int>::max)()));
			const auto size = static_cast<int>(N - 1);  // bin2c appends a NUL terminator
			const auto* data = reinterpret_cast<const char*>(a_data);
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
		static const auto bridge = LoadScript(kBridgeShim);
		static const auto network = LoadScript(kNetworkGuard);
		switch (a_script) {
		case EmbeddedScript::BridgeShim:   return bridge;
		case EmbeddedScript::NetworkGuard: return network;
		}
		return bridge;
	}
}
