#pragma once

// osfui_webview2_host.exe — out-of-process WebView2 host for OSF UI.
//
// Owns the WebView2 environment/composition controller, the Windows.Graphics.
// Capture session over the composition visual, and a ring of NT-handle shared
// D3D11 textures the game composites directly (shared-fence synchronized).
// The game plugin (or the standalone POC client) is the pipe server; this
// process is launched OUT OF THE GAME'S PROCESS TREE (Wv2BrokerLaunch) so
// MO2's USVFS never injects into the browser processes it spawns.
//
// Lifetime: exits when the pipe breaks, when a shutdown message arrives, or
// when the game process handle signals. Never orphans: the game PID is a
// required argument and is waited on in the main pump.

#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

namespace osfui::wv2
{
	class Pipe;

	struct HostOptions
	{
		std::wstring          pipeName;      // without \\.\pipe\ prefix
		std::uint32_t         gamePid{ 0 };
		std::filesystem::path logFile;       // empty = no file log
	};

	// Returns the process exit code.
	int RunHost(const HostOptions& a_options);
}
