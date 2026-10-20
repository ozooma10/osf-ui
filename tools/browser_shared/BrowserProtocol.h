#pragma once


#include <cstdint>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace osfui::browser
{
	inline constexpr std::uint32_t kBrowserHostProtocolVersion = 8;

	// Mandatory wire behavior shared by both implementations.
    inline constexpr std::uint32_t kSharedTextureRing = 1u << 0;
    inline constexpr std::uint32_t kFrameAcknowledgments = 1u << 1;
    inline constexpr std::uint32_t kPresentationEpochs = 1u << 2;
    inline constexpr std::uint32_t kRequiredHostCapabilities =
        kSharedTextureRing | kFrameAcknowledgments | kPresentationEpochs;

    inline constexpr std::uint32_t kHelloTimeoutMs = 10000;
	inline constexpr std::uint32_t kHeartbeatIntervalMs = 1000;
	inline constexpr std::uint32_t kHeartbeatTimeoutMs = 10000;

	// Pipe name pattern: \\.\pipe\osfui-browser-<gamePid>-<nonce>
	inline constexpr const wchar_t* kPipePrefix = L"osfui-browser-";

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
	inline constexpr std::uint32_t kMaxPageMessageBytes = 64u * 1024u;
	inline constexpr std::uint32_t kMaxPageMessagesPerSecond = 128;

	inline constexpr std::uint32_t kRingSlots = 4;

	inline constexpr std::uint32_t kDefaultLogicalHeight = 900;

}
