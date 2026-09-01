#include "Runtime/AdaptiveViewGeometry.h"

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

	bool Is(const OSFUI::ViewSize a_size, const std::uint32_t a_width,
		const std::uint32_t a_height)
	{
		return a_size.width == a_width && a_size.height == a_height;
	}
}

int main()
{
	using OSFUI::ViewSizeForOutput;

	Check(Is(ViewSizeForOutput({ 3440, 1440 }, false), 3440, 1440),
		"ordinary ultrawide presentation stays full width");
	Check(Is(ViewSizeForOutput({ 3440, 1440 }, true), 2560, 1440),
		"Chargen contains a 16:9 browser surface inside 3440x1440");
	Check(Is(ViewSizeForOutput({ 2560, 1080 }, true), 1920, 1080),
		"render-scaled ultrawide preserves the fixed 16:9 presentation aspect");
	Check(Is(ViewSizeForOutput({ 1920, 1080 }, false), 1920, 1080) &&
		Is(ViewSizeForOutput({ 1920, 1080 }, true), 1920, 1080),
		"a native 16:9 output needs no geometry change");
	Check(Is(ViewSizeForOutput({ 1280, 1024 }, true), 1280, 720),
		"narrow output letterboxes rather than cropping the fixed aspect");
	Check(Is(ViewSizeForOutput({ 0, 1440 }, true), 0, 1440) &&
		Is(ViewSizeForOutput({ 3440, 0 }, true), 3440, 0),
		"invalid output dimensions remain invalid for the caller to reject");

	const OSFUI::ViewSize packedSource{ 2560, 1440 };
	const auto unpacked = OSFUI::UnpackViewSize(OSFUI::PackViewSize(packedSource));
	Check(Is(unpacked, 2560, 1440),
		"packed geometry publishes width and height as one coherent snapshot");

	if (failures == 0) {
		std::cout << "adaptive_view_geometry_tests: ok\n";
	}
	return failures;
}
