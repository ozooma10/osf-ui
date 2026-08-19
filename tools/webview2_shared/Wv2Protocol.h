#pragma once


#include <cstdint>

namespace osfui::wv2
{
	inline constexpr std::uint32_t kBrowserHostProtocolVersion = 8;

	inline constexpr std::uint32_t kHelloTimeoutMs = 10000;
	inline constexpr std::uint32_t kHeartbeatIntervalMs = 1000;
	inline constexpr std::uint32_t kHeartbeatTimeoutMs = 10000;

	// Pipe name pattern: \\.\pipe\osfui-wv2-<gamePid>-<nonce>
	inline constexpr const wchar_t* kPipePrefix = L"osfui-wv2-";

	inline constexpr std::uint32_t kRestoreGameFocusMessage = 0x8049;

	inline constexpr std::uint32_t kMaxMessageBytes = 8u * 1024u * 1024u;

	inline constexpr std::uint32_t kRingSlots = 4;

	inline constexpr std::uint32_t kDefaultLogicalHeight = 900;

}
