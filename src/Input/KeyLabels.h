#pragma once

#include "Input/InputTypes.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace OSFUI
{
	// Inject platform keycap facts so label building remains desktop-testable.
	struct KeyLabelSource
	{
		// Current-layout UTF-8 keycap glyph; dead keys return their spacing accent.
		std::function<std::string(ScanCode)> glyph;
		// Last-resort layout-DLL name with left and right modifiers kept distinct.
		std::function<std::string(ScanCode)> layoutName;
		// Locale tag of the active layout ("de-DE"); "" when undetermined.
		std::function<std::string()> layoutTag;
	};

	// Resolve non-printing keys through the UI locale, falling back to authored English.
	using KeyTextResolver =
		std::function<std::string(std::string_view a_address, std::string_view a_english)>;

	struct KeyLabels
	{
		// Layout tag for diagnostics/display ("de-DE"); "" when unknown.
		std::string layout;
		// Scan-ordered display labels are never binding identities or persisted values.
		std::vector<std::pair<std::string, std::string>> labels;

		bool operator==(const KeyLabels&) const = default;
	};

	// Build a nonempty label from localized fixed text, layout glyph/name, then the canonical key name.
	[[nodiscard]] KeyLabels BuildKeyLabels(const KeyLabelSource& a_source, const KeyTextResolver& a_text);
}
