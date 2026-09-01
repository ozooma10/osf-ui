#include "Input/AbsoluteMouseMapping.h"

#include <cmath>
#include <iostream>

namespace
{
	int failures = 0;

	void Check(const bool a_condition, const char* a_message)
	{
		if (!a_condition) {
			std::cerr << "FAIL: " << a_message << '\n';
			++failures;
		}
	}

	bool Near(const float a_left, const float a_right)
	{
		return std::abs(a_left - a_right) < 0.01f;
	}
}

int main()
{
	auto mapped = OSFUI::MapAbsoluteMouseToView(440, 720, 3440, 1440, 2560, 1440);
	Check(mapped.inside && Near(mapped.x, 0.0f) && Near(mapped.y, 720.0f),
		"the left edge of a centered 16:9 target maps to view x=0");

	mapped = OSFUI::MapAbsoluteMouseToView(1720, 720, 3440, 1440, 2560, 1440);
	Check(mapped.inside && Near(mapped.x, 1280.0f) && Near(mapped.y, 720.0f),
		"the client center maps to the centered target center");

	mapped = OSFUI::MapAbsoluteMouseToView(100, 720, 3440, 1440, 2560, 1440);
	Check(!mapped.inside && Near(mapped.x, 0.0f),
		"the pillarbox area is outside the view and clamps to its edge");

	mapped = OSFUI::MapAbsoluteMouseToView(3000, 720, 3440, 1440, 2560, 1440);
	Check(!mapped.inside && Near(mapped.x, 2559.0f),
		"the right pillarbox area cannot generate an in-view click");

	mapped = OSFUI::MapAbsoluteMouseToView(2999, 1439, 3440, 1440, 2560, 1440);
	Check(mapped.inside && Near(mapped.x, 2559.0f) && Near(mapped.y, 1439.0f),
		"the centered viewport uses half-open right and bottom edges");

	mapped = OSFUI::MapAbsoluteMouseToView(1720, 720, 3440, 1440, 1720, 720);
	Check(mapped.inside && Near(mapped.x, 860.0f) && Near(mapped.y, 360.0f),
		"a uniformly scaled target preserves proportional input mapping");

	mapped = OSFUI::MapAbsoluteMouseToView(1712, 705, 3425, 1410, 2560, 1440);
	Check(mapped.inside && Near(mapped.x, 1279.49f) && Near(mapped.y, 720.0f),
		"the captured ultrawide client maps through its fractional centered viewport");

	mapped = OSFUI::MapAbsoluteMouseToView(640, 100, 1280, 1024, 1280, 720);
	Check(!mapped.inside && Near(mapped.y, 0.0f),
		"vertical letterbox input is rejected and clamped");
	mapped = OSFUI::MapAbsoluteMouseToView(640, 152, 1280, 1024, 1280, 720);
	Check(mapped.inside && Near(mapped.x, 640.0f) && Near(mapped.y, 0.0f),
		"the top edge of a vertically contained view is inside");

	mapped = OSFUI::MapAbsoluteMouseToView(10, 10, 0, 1440, 2560, 1440);
	Check(!mapped.inside && Near(mapped.x, 0.0f) && Near(mapped.y, 0.0f),
		"invalid client geometry produces no usable mapping");

	if (failures == 0) {
		std::cout << "absolute_mouse_mapping_tests: ok\n";
	}
	return failures;
}
