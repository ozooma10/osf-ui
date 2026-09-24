#include "World/WorldAssets.h"
#include "Composite/D3D12Prologue.h"
#include <array>
#include <bcrypt.h>

namespace OSFUI::WorldAssets
{
	std::string TexturePath(std::string_view qualifiedID)
	{
		std::array<UCHAR, 32> digest{};
		if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
				reinterpret_cast<PUCHAR>(const_cast<char*>(qualifiedID.data())), static_cast<ULONG>(qualifiedID.size()),
				digest.data(), static_cast<ULONG>(digest.size())) < 0)
			return {};
		constexpr char hex[] = "0123456789abcdef";
		std::string    path = "textures/osfui/feeds/";
		for (std::size_t i = 0; i < digest.size(); ++i) {
			if (i == 16)
				path += '/';
			path += hex[digest[i] >> 4];
			path += hex[digest[i] & 15];
		}
		return path + ".dds";
	}
}