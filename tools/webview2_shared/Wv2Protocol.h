#pragma once


#include <cstdint>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace osfui::wv2
{
	inline constexpr std::uint32_t kBrowserHostProtocolVersion = 18;

	inline constexpr std::uint32_t kHelloTimeoutMs = 10000;
	inline constexpr std::uint32_t kHeartbeatIntervalMs = 1000;
	inline constexpr std::uint32_t kHeartbeatTimeoutMs = 10000;

	// Pipe name pattern: \\.\pipe\osfui-wv2-<gamePid>-<nonce>
	inline constexpr const wchar_t* kPipePrefix = L"osfui-wv2-";

	inline constexpr const wchar_t* kRestoreGameFocusMessageName = L"OSFUI.RestoreGameFocus.v2";
	inline constexpr const wchar_t* kRefreshInputStateMessageName = L"OSFUI.RefreshInputState.v2";
#ifdef _WIN32
	inline UINT RestoreGameFocusMessage()
	{
		static const UINT message = ::RegisterWindowMessageW(kRestoreGameFocusMessageName);
		return message;
	}
	inline UINT RefreshInputStateMessage()
	{
		static const UINT message = ::RegisterWindowMessageW(kRefreshInputStateMessageName);
		return message;
	}
#endif

	inline constexpr std::uint32_t kMaxMessageBytes = 8u * 1024u * 1024u;

	inline constexpr std::uint32_t kRingSlots = 4;

	inline constexpr std::uint32_t kDefaultLogicalHeight = 900;

}
