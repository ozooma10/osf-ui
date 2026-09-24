#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "Core/StringUtil.h"

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

	[[nodiscard]] constexpr bool PostCompositeTargetMatchesOutputAspect(
		const std::uint64_t a_targetWidth,
		const std::uint32_t a_targetHeight,
		const std::uint32_t a_outputWidth,
		const std::uint32_t a_outputHeight)
	{
		if (a_targetWidth == 0 || a_targetHeight == 0 || a_targetWidth > 0xFFFF'FFFFull) {
			return false;
		}
		if (a_outputWidth == 0 || a_outputHeight == 0) {
			return true;
		}

		// Render scale changes target dimensions without changing presentation
		// geometry. Allow one target-pixel of cross-product rounding.
		const auto targetCross = a_targetWidth * static_cast<std::uint64_t>(a_outputHeight);
		const auto outputCross = static_cast<std::uint64_t>(a_outputWidth) * a_targetHeight;
		const auto difference = targetCross > outputCross ?
			targetCross - outputCross : outputCross - targetCross;
		const auto onePixelTolerance = static_cast<std::uint64_t>(
			a_outputWidth > a_outputHeight ? a_outputWidth : a_outputHeight);
		return difference <= onePixelTolerance;
	}

	enum class PostCompositeTargetFormat
	{
		Rgba8,
		Rgba16Float,
	};

	struct PostCompositeTargetSelection
	{
		bool                      supported{ false };
		PostCompositeTargetFormat format{ PostCompositeTargetFormat::Rgba8 };
	};

	// Vanilla uses ScaleformEnd so generated and rendered frames receive the same
	// overlay. Luma owns a proven RGBA16F post-composite surface; other foreign
	// owners have no proven target contract and also use ScaleformEnd.
	[[nodiscard]] constexpr PostCompositeTargetSelection SelectPostCompositeTarget(
		const bool a_vanillaComposite,
		const std::string_view a_foreignOwner)
	{
		if (a_vanillaComposite) {
			return {};
		}
		if (StringUtil::EqualsCaseInsensitiveAscii(a_foreignOwner, "Luma.dll")) {
			return { true, PostCompositeTargetFormat::Rgba16Float };
		}
		return {};
	}

	class ScaleformHandoffWindow final
	{
	public:
		void Begin()
		{
			m_trackingHeaps = true;
			m_handoffsLeft = 0;
			m_barrierCalls = 0;
		}

		void End()
		{
			if (m_trackingHeaps) {
				m_handoffsLeft = 2;
				m_barrierCalls = 0;
			}
		}

		void OnBarrierCall()
		{
			if (m_handoffsLeft > 0 && ++m_barrierCalls > 4) {
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
			// Both the first candidate and the second must fit inside this bounded window.
			if (m_handoffsLeft == 0) {
				m_trackingHeaps = false;
			}
			return regionFirst;
		}

		void Cancel()
		{
			m_trackingHeaps = false;
			m_handoffsLeft = 0;
			m_barrierCalls = 0;
		}

		[[nodiscard]] bool TrackingHeaps() const { return m_trackingHeaps; }
		[[nodiscard]] bool HandoffArmed() const { return m_handoffsLeft > 0; }

	private:
		bool m_trackingHeaps{ false };
		int  m_handoffsLeft{ 0 };
		int  m_barrierCalls{ 0 };
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
