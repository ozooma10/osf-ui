#pragma once

#include "World/WorldAssets.h"
#include <optional>

namespace OSFUI::EngineTextureIdentity
{
	// The returned key is valid only during this thread's verified TextureDB
	// creation call. No global last-loaded path and no dimensions are involved.
	std::optional<WorldAssets::Key> CurrentAsset();
	bool                            Install();
}