#include "render/IWebRenderer.h"

#include <cassert>
#include <iostream>

int main()
{
	using OSFUI::CursorShape;
	using OSFUI::CursorShapeFromSystemCursorId;

	assert(CursorShapeFromSystemCursorId(32512) == CursorShape::kArrow);
	assert(CursorShapeFromSystemCursorId(32513) == CursorShape::kIBeam);
	assert(CursorShapeFromSystemCursorId(32514) == CursorShape::kWait);
	assert(CursorShapeFromSystemCursorId(32515) == CursorShape::kCross);
	assert(CursorShapeFromSystemCursorId(32642) == CursorShape::kSizeNWSE);
	assert(CursorShapeFromSystemCursorId(32643) == CursorShape::kSizeNESW);
	assert(CursorShapeFromSystemCursorId(32644) == CursorShape::kSizeAll);
	assert(CursorShapeFromSystemCursorId(32645) == CursorShape::kSizeWE);
	assert(CursorShapeFromSystemCursorId(32646) == CursorShape::kSizeNS);
	assert(CursorShapeFromSystemCursorId(32648) == CursorShape::kNotAllowed);
	assert(CursorShapeFromSystemCursorId(32649) == CursorShape::kHand);
	assert(CursorShapeFromSystemCursorId(32651) == CursorShape::kHelp);
	assert(CursorShapeFromSystemCursorId(0) == CursorShape::kNone);
	assert(CursorShapeFromSystemCursorId(99999) == CursorShape::kArrow);

	std::cout << "cursor shape tests passed\n";
}
