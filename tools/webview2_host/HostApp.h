#pragma once


#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
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
	};

	// Returns the process exit code.
	int RunHost(const HostOptions& a_options);
}
