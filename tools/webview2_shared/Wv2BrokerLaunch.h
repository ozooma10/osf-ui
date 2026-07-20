#pragma once

// Launches an executable outside the calling process's tree so Mod Organizer
// 2's USVFS (which hooks CreateProcessInternalW in the game and injects a
// trampoline into every child — crashing msedgewebview2.exe's broker) never
// sees it. Two out-of-tree brokers are tried in order, then direct spawn:
//
//   1. Explorer: IShellDispatch2::ShellExecute through the running desktop
//      shell — the new process becomes a child of explorer.exe.
//   2. Task Scheduler: a one-shot interactive task, run + deleted immediately.
//   3. Direct CreateProcess — correct when usvfs_x64.dll is not loaded
//      (vanilla / Vortex installs); also the last resort under MO2 (it will
//      inject, but a dead overlay beats no attempt plus a clear log line).
//
// No PID is returned: brokered launches cannot observe it reliably. The host
// identifies itself over the pipe (hello.pid) instead.
//
// Shared plugin/tool code, so it does no logging: the result carries the
// method used and per-method failure details for the caller to log.

#include <string>

namespace osfui::wv2
{
	enum class LaunchMethod
	{
		kNone,
		kExplorer,
		kTaskScheduler,
		kDirect,
	};

	struct LaunchResult
	{
		bool         ok{ false };
		LaunchMethod method{ LaunchMethod::kNone };
		std::string  detail;  // per-attempt diagnostics (also on success)
	};

	[[nodiscard]] const char* LaunchMethodName(LaunchMethod a_method);

	// a_exe must be a real filesystem path visible to Explorer / the task
	// scheduler service, never a VFS-only path. a_args is the raw argument
	// string (caller quotes). a_preferBroker=false skips straight to direct
	// CreateProcess (the usvfs-absent fast path).
	[[nodiscard]] LaunchResult LaunchDetached(const std::wstring& a_exe,
		const std::wstring& a_args, bool a_preferBroker);
}
