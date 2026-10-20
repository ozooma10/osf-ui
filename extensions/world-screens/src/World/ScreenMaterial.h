#pragma once

#include <cstdint>
#include <string_view>

namespace RE { class NiAVObject; }

namespace OSFUI::ScreenMaterial
{
	bool Supported();
	bool FullUV(RE::NiAVObject* a_root);
	// Only inside the engine material-setup callback. Geometry owns the loaded
	// resource; callers retain no MaterialDB entries or scene pointers.
	bool Apply(RE::NiAVObject* a_root, std::string_view a_materialPath);
	std::uint32_t Current(RE::NiAVObject* a_root);
	bool ApplyID(RE::NiAVObject* a_root, std::uint32_t a_id);
}
