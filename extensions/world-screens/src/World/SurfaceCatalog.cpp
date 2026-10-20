#include "World/SurfaceCatalog.h"
#include "World/WorldAssets.h"
#include "Core/StringUtil.h"

#include <algorithm>

namespace OSFUI::WorldTexture
{
	SurfaceCatalog SelectSupportedSurfaces(std::span<const SurfaceDesc> descriptions)
	{
		SurfaceCatalog result;
		for (const auto& desc : descriptions) {
			if (desc.id.empty() || !WorldAssets::IsGeneratedPath(desc.texture) ||
				desc.texture != WorldAssets::TexturePath(desc.id) ||
				desc.width == 0 || desc.height == 0 || desc.width > 4096 || desc.height > 4096) {
				result.rejected.push_back({desc.id, "invalid or foreign generated texture binding"});
				continue;
			}
			const auto duplicate = std::ranges::any_of(result.accepted, [&](const auto& accepted) {
				return StringUtil::EqualsCaseInsensitiveAscii(accepted.id, desc.id) ||
					WorldAssets::KeyForPath(accepted.texture) == WorldAssets::KeyForPath(desc.texture);
			});
			if (duplicate) {
				result.rejected.push_back({desc.id, "duplicate feed or asset claim"});
				continue;
			}
			result.accepted.push_back(desc);
		}
		return result;
	}
}
