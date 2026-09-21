#pragma once

#include <filesystem>
#include <string>

namespace OSFUI
{
	inline std::string Utf8Path(const std::filesystem::path& a_path)
	{
		const auto utf8 = a_path.generic_u8string();
		return { utf8.begin(), utf8.end() };
	}
}
