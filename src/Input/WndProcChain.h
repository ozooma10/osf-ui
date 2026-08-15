#pragma once

#include <cstdint>

namespace OSFUI::OverlayInputHook::detail
{
	// A hook that was below us when we installed can later put itself back at
	// the top of the window-procedure chain. Calling our saved "original" then
	// calls the current top-level hook, which can chain straight back to us.
	[[nodiscard]] constexpr bool OriginalMovedAboveUs(
		const std::uintptr_t a_current,
		const std::uintptr_t a_ours,
		const std::uintptr_t a_original)
	{
		return a_current != a_ours && a_current == a_original;
	}
}
