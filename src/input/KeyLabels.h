#pragma once

#include "input/InputTypes.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace OSFUI
{
	// Platform facts for localized keycap labels, injected as resolvers so the
	// builder stays native-desktop-testable (fakes in
	// tests/native/key_label_tests.cpp; the real thing is
	// Platform::MakeKeyLabelSource).
	struct KeyLabelSource
	{
		// The keycap glyph the CURRENT layout prints on a physical key, UTF-8,
		// in keycap (uppercase) form — "Ö" for the German semicolon-position
		// key. Dead keys return their spacing accent ("^", "´"). "" = the key
		// produces no printable glyph. Wraps ToUnicodeEx with the
		// do-not-change-keyboard-state flag.
		std::function<std::string(ScanCode)> glyph;
		// The layout DLL's display name for a key — last-resort fallback only
		// (its casing and language are inconsistent across layouts). Wraps
		// GetKeyNameTextW with the sided-modifier "don't care" bit CLEAR, so
		// left/right stay distinct.
		std::function<std::string(ScanCode)> layoutName;
		// Locale tag of the active layout ("de-DE"); "" when undetermined.
		std::function<std::string()> layoutTag;
	};

	// Resolves a chrome.keys.<Name> catalog address to the UI locale, falling
	// back to the given English default (Runtime wires LocalizationService).
	// Non-printing keys go through this instead of the layout DLL: the UI
	// language is the player's choice, the layout's language is not.
	using KeyTextResolver =
		std::function<std::string(std::string_view a_address, std::string_view a_english)>;

	struct KeyLabels
	{
		// Layout tag for diagnostics/display ("de-DE"); "" when unknown.
		std::string layout;
		// name -> label, one row per nameable physical key, scan-code order.
		// DISPLAY ONLY — a label is never an identity and never persisted.
		std::vector<std::pair<std::string, std::string>> labels;

		bool operator==(const KeyLabels&) const = default;
	};

	// One row per scan code KeyName() can name. Per key: fixed English short
	// forms (through the text resolver) for non-printing keys, the name itself
	// for F-keys, the layout glyph for printable keys, then the layout-DLL
	// name, then the key name — a label is never empty.
	[[nodiscard]] KeyLabels BuildKeyLabels(const KeyLabelSource& a_source, const KeyTextResolver& a_text);
}
