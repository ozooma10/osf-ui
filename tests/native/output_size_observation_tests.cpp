#include "Composite/OutputSizeObservation.h"

#include <cstdint>
#include <iostream>
#include <limits>

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
}

int main()
{
	OSFUI::OutputSizeObservation observation;
	Check(!observation.Snapshot(), "the output size starts unknown");

	observation.Publish(0, 1080);
	observation.Publish(1920, 0);
	Check(!observation.Snapshot(), "zero-sized targets are ignored");

	observation.Publish(1920, 1080);
	auto size = observation.Snapshot();
	Check(size && size->width == 1920 && size->height == 1080,
		"a published size is observed as one coherent value");

	observation.Publish((std::numeric_limits<std::uint32_t>::max)(), 1440);
	size = observation.Snapshot();
	Check(size && size->width == (std::numeric_limits<std::uint32_t>::max)() && size->height == 1440,
		"the full width range is preserved");

	if (failures == 0) {
		std::cout << "output_size_observation_tests: ok\n";
	}
	return failures;
}
