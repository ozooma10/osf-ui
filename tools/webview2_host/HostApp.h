#pragma once

// osfui_webview2_host.exe — out-of-process WebView2 host for OSF UI. Owns the
// WebView2 environment/composition controller, the Windows.Graphics.Capture
// session over the composition visual, and a ring of NT-handle shared D3D11
// textures the game composites directly (shared-fence synchronized).
//
// The plugin is the pipe server; this process is launched outside the game's
// process tree (Wv2BrokerLaunch) so MO2's USVFS can't inject into the browser
// processes it spawns.
//
// Exits when the pipe breaks, on a shutdown message, when the game process
// handle signals, or when its top-level window remains absent. Post-exit dialogs
// are time-bounded as well, so neither a missed watcher signal nor an unattended
// prompt can orphan the host.

#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
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
		std::wstring          reportEndpoint; // empty = abnormal-exit prompt disabled
		std::filesystem::path reportPluginRoot; // installed root to redact from logs
#if defined(OSFUI_WITH_WORLD_SURFACES)
		// Empty = primary overlay instance. A non-empty tag scopes the
		// single-instance lock so an independent second host (world surface)
		// can run for the same game process.
		std::wstring          instance;
#endif
	};

	// Returns the process exit code.
	int RunHost(const HostOptions& a_options);
}
