#include "World/ScreenFormId.h"

namespace OSFUI::ScreenFormId
{
	std::optional<std::uint32_t> Compose(Tier tier, std::uint32_t slot, std::uint32_t local)
	{
		switch (tier) {
		case Tier::Full:
			if (slot > 0xFC || local > 0xFFFFFF) return std::nullopt;
			return (slot << 24) | local;
		case Tier::Medium:
			if (slot > 0xFF || local > 0xFFFF) return std::nullopt;
			return 0xFD000000u | (slot << 16) | local;
		case Tier::Small:
			if (slot > 0xFFF || local > 0xFFF) return std::nullopt;
			return 0xFE000000u | (slot << 12) | local;
		}
		return std::nullopt;
	}
}
