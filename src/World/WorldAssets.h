#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace OSFUI::WorldAssets
{
	struct Key
	{
		std::uint32_t file{ 0 }, extension{ 0 }, directory{ 0 };
		bool          operator==(const Key&) const = default;
	};
	static_assert(sizeof(Key) == 12);

	// BSResource::ID for our canonical DDS paths. Verified against TextureDB
	// entries in 1.16.244.0. CRC starts at zero and has no final XOR.
	inline std::uint32_t PathCRC(std::string_view text)
	{
		std::uint32_t crc = 0;
		for (unsigned char byte : text) {
			if (byte >= 'A' && byte <= 'Z')
				byte += 'a' - 'A';
			if (byte == '/')
				byte = '\\';
			crc ^= byte;
			for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
		}
		return crc;
	}

	inline Key KeyForPath(std::string_view path)
	{
		const auto slash = path.find_last_of("/\\");
		const auto dot = path.find_last_of('.');
		if (slash == path.npos || dot == path.npos || dot <= slash)
			return {};
		return { PathCRC(path.substr(slash + 1, dot - slash - 1)), 0x00736464u, PathCRC(path.substr(0, slash)) };
	}

	inline bool IsGeneratedPath(std::string_view path)
	{
		constexpr std::string_view prefix = "textures/osfui/feeds/";
		if (!path.starts_with(prefix) || path.size() != prefix.size() + 32 + 1 + 32 + 4 || !path.ends_with(".dds"))
			return false;
		path.remove_prefix(prefix.size());
		for (std::size_t i = 0; i < 65; ++i) {
			if (i == 32) {
				if (path[i] != '/')
					return false;
			} else if (!((path[i] >= '0' && path[i] <= '9') || (path[i] >= 'a' && path[i] <= 'f')))
				return false;
		}
		return true;
	}

	// SHA-256 of the exact UTF-8 qualified feed ID, split across directory/file
	// so both halves of the engine's resource key vary between feeds.
	std::string TexturePath(std::string_view qualifiedID);
}