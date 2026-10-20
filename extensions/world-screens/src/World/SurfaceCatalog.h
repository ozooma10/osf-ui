#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace OSFUI::WorldTexture
{
	struct SurfaceDesc
	{
		std::string   id;
		std::string   texture;
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
	};

	struct RejectedSurface
	{
		std::string id;
		const char* reason;
	};

	struct SurfaceCatalog
	{
		std::vector<SurfaceDesc> accepted;
		std::vector<RejectedSurface> rejected;
	};

	// Content errors affect only their own feed. Accepted indices keep discovery
	// order; the first valid claim wins if a later feed duplicates its identity.
	SurfaceCatalog SelectSupportedSurfaces(std::span<const SurfaceDesc> descriptions);
}
