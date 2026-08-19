#pragma once

namespace OSFUI
{
	enum class CursorShape
	{
		kArrow,
		kCross,
		kHand,
		kIBeam,
		kWait,
		kHelp,
		kNotAllowed,
		kSizeWE,
		kSizeNS,
		kSizeNESW,
		kSizeNWSE,
		kSizeAll,
		kNone,
	};

	[[nodiscard]] constexpr CursorShape CursorShapeFromSystemCursorId(std::uint32_t a_id) noexcept
	{
		switch (a_id) {
		case 0:     return CursorShape::kNone;
		case 32513: return CursorShape::kIBeam;
		case 32514: return CursorShape::kWait;
		case 32515: return CursorShape::kCross;
		case 32642: return CursorShape::kSizeNWSE;
		case 32643: return CursorShape::kSizeNESW;
		case 32644: return CursorShape::kSizeAll;
		case 32645: return CursorShape::kSizeWE;
		case 32646: return CursorShape::kSizeNS;
		case 32648: return CursorShape::kNotAllowed;
		case 32649: return CursorShape::kHand;
		case 32651: return CursorShape::kHelp;
		default:    return CursorShape::kArrow;
		}
	}
}
