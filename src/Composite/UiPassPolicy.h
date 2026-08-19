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

	class ScaleformHandoffWindow final
	{
	public:
		void Begin()
		{
			m_trackingHeaps = true;
			m_handoffsLeft = 0;
			m_barrierCallsAfterFirst = -1;
		}

		void End()
		{
			if (m_trackingHeaps) {
				m_handoffsLeft = 2;
				m_barrierCallsAfterFirst = -1;
			}
		}

		void OnBarrierCall()
		{
			if (m_handoffsLeft > 0 && m_barrierCallsAfterFirst >= 0 && ++m_barrierCallsAfterFirst > 4) {
				Cancel();
			}
		}

		[[nodiscard]] bool ConsumeAndReportFirstCandidate()
		{
			const bool regionFirst = m_handoffsLeft == 2;
			if (m_handoffsLeft <= 0) {
				return false;
			}
			m_handoffsLeft--;
			if (m_barrierCallsAfterFirst < 0) {
				m_barrierCallsAfterFirst = 0;
			}
			if (m_handoffsLeft == 0) {
				m_trackingHeaps = false;
			}
			return regionFirst;
		}

		void Cancel()
		{
			m_trackingHeaps = false;
			m_handoffsLeft = 0;
			m_barrierCallsAfterFirst = -1;
		}

		[[nodiscard]] bool TrackingHeaps() const { return m_trackingHeaps; }
		[[nodiscard]] bool HandoffArmed() const { return m_handoffsLeft > 0; }

	private:
		bool m_trackingHeaps{ false };
		int  m_handoffsLeft{ 0 };
		int  m_barrierCallsAfterFirst{ -1 };
	};

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
				const bool previousRegionHadFgTarget = _regionSawFgTarget.exchange(false, std::memory_order_acq_rel);
				_frameGeneration.store(previousRegionHadFgTarget, std::memory_order_release);
				classificationKnown = _classificationKnown.exchange(true, std::memory_order_acq_rel);
			}
			if (a_fgTarget) {
				_regionSawFgTarget.store(true, std::memory_order_release);
				_frameGeneration.store(true, std::memory_order_release);
			}

			const bool frameGeneration = _frameGeneration.load(std::memory_order_acquire);
			const bool draw = (classificationKnown || a_fgTarget) && (!frameGeneration || a_fgTarget);
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
