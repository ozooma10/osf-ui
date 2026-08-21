#include "Wv2MouseButtons.h"

#include <cassert>
#include <string>

using namespace osfui::wv2;

int main()
{
	SyntheticMouseButtons buttons;
	assert(buttons.Pressed() == 0);
	assert(buttons.TakePressed() == 0);  // clean boundaries produce no recovery diagnostic

	buttons.Observe(SyntheticMouseButton::left, true);
	buttons.Observe(SyntheticMouseButton::left, true);  // repeated down remains one pressed bit
	buttons.Observe(SyntheticMouseButton::right, true);
	assert(buttons.IsPressed(SyntheticMouseButton::left));
	assert(buttons.IsPressed(SyntheticMouseButton::right));
	assert(!buttons.IsPressed(SyntheticMouseButton::middle));

	buttons.Observe(SyntheticMouseButton::left, false);
	assert(!buttons.IsPressed(SyntheticMouseButton::left));
	assert(buttons.IsPressed(SyntheticMouseButton::right));
	assert(buttons.TakePressed() == MouseButtonBit(SyntheticMouseButton::right));
	assert(buttons.Pressed() == 0);
	assert(buttons.TakePressed() == 0);  // recovery is reported exactly once

	buttons.Observe(SyntheticMouseButton::middle, true);
	buttons.Observe(SyntheticMouseButton::left, true);
	const auto stuck = buttons.TakePressed();
	std::string releaseOrder;
	for (const auto button : kSyntheticMouseReleaseOrder) {
		if ((stuck & MouseButtonBit(button)) == 0) continue;
		if (!releaseOrder.empty()) releaseOrder += ',';
		releaseOrder += MouseButtonName(button);
	}
	assert(releaseOrder == "left,middle");

	buttons.Observe(SyntheticMouseButton::right, false);  // unmatched up stays clean
	assert(buttons.Pressed() == 0);
}
