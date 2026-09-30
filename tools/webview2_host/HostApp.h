#pragma once


#include <cstdint>
#include <filesystem>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

namespace osfui::wv2
{
	struct HostOptions
	{
		std::wstring          pipeName;
		std::uint32_t         gamePid{ 0 };
		std::filesystem::path logFile;       // empty = no file log
	};

	// Returns the process exit code.
	int RunHost(const HostOptions& a_options);
}
