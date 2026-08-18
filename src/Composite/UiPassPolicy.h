#pragma once

#include <cstdint>

namespace OSFUI::UiPass::detail
{
	enum class CommandListHookState
	{
		Uninitialized,
		Installing,
		Ready,
		Failed,
	};

	[[nodiscard]] constexpr bool CanRecordOverlay(const CommandListHookState a_state)
	{
		return a_state == CommandListHookState::Ready;
	}

	// Fail-open: a foreign pointer in an execute slot is assumed to be a call-through hook and chained
	[[nodiscard]] constexpr bool CanChainForeignExecute(const std::uintptr_t a_current)
	{
		return a_current != 0;
	}
}
