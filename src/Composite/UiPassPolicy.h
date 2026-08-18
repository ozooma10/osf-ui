#pragma once

#include <atomic>
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

	struct TargetDecision
	{
		bool draw{ false };
		bool firstDrawInRegion{ false };
		bool frameGeneration{ false };
	};

	class FrameGenerationTargetPolicy final
	{
	public:
		TargetDecision Observe(bool a_fgTarget, bool a_regionFirst)
		{
			bool classificationKnown = _classificationKnown.load(std::memory_order_acquire);
			if (a_regionFirst) {
				const bool previousRegionHadFgTarget =
					_regionSawFgTarget.exchange(false, std::memory_order_acq_rel);
				_frameGeneration.store(previousRegionHadFgTarget, std::memory_order_release);
				classificationKnown = _classificationKnown.exchange(true, std::memory_order_acq_rel);
			}
			if (a_fgTarget) {
				_regionSawFgTarget.store(true, std::memory_order_release);
				_frameGeneration.store(true, std::memory_order_release);
			}

			const bool frameGeneration = _frameGeneration.load(std::memory_order_acquire);
			const bool draw = (classificationKnown || a_fgTarget) &&
				(!frameGeneration || a_fgTarget);
			return {
				.draw = draw,
				.firstDrawInRegion = draw && (a_regionFirst || (frameGeneration && a_fgTarget)),
				.frameGeneration = frameGeneration,
			};
		}

		bool FrameGenerationActive() const
		{
			return _frameGeneration.load(std::memory_order_acquire);
		}

	private:
		std::atomic_bool _frameGeneration{ false };
		std::atomic_bool _regionSawFgTarget{ false };
		std::atomic_bool _classificationKnown{ false };
	};
}
