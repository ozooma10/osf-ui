#pragma once

#include <cstddef>
#include <string_view>

namespace OSFUI::UiPassSeam::detail
{
	enum class ExecuteSlotKind
	{
		Begin,
		End,
		Composite,
	};

	[[nodiscard]] constexpr char FoldAscii(const char a_character)
	{
		return a_character >= 'A' && a_character <= 'Z' ?
			static_cast<char>(a_character + ('a' - 'A')) : a_character;
	}

	[[nodiscard]] constexpr bool EqualsAsciiInsensitive(
		const std::string_view a_left,
		const std::string_view a_right)
	{
		if (a_left.size() != a_right.size()) {
			return false;
		}
		for (std::size_t i = 0; i < a_left.size(); ++i) {
			if (FoldAscii(a_left[i]) != FoldAscii(a_right[i])) {
				return false;
			}
		}
		return true;
	}

	// Luma ebe4a52 hooks this exact pass, captures its target, then calls the
	// previous slot owner. OSF UI installs after every SFSE plugin has loaded, so
	// Luma has already patched the vanilla implementation it also edits at a
	// fixed offset. No other foreign owner is ABI- or lifecycle-proven and
	// remains fail-closed.
	[[nodiscard]] constexpr bool CanChainForeignExecute(
		const ExecuteSlotKind a_slot,
		const std::string_view a_owner)
	{
		return a_slot == ExecuteSlotKind::Composite &&
			EqualsAsciiInsensitive(a_owner, "Luma.dll");
	}
}
