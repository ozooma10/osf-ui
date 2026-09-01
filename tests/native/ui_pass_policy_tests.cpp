#include "Composite/UiPassPolicy.h"

#include <iostream>

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
	using OSFUI::UiPass::detail::CanChainForeignExecute;
	using OSFUI::UiPass::detail::CanRecordOverlay;
	using OSFUI::UiPass::detail::CommandListHookState;
	using OSFUI::UiPass::detail::FrameGenerationTargetPolicy;
	using OSFUI::UiPass::detail::PostCompositeTargetMatchesOutputAspect;
	using OSFUI::UiPass::detail::PostCompositeTargetFormat;
	using OSFUI::UiPass::detail::ScaleformHandoffWindow;
	using OSFUI::UiPass::detail::SelectPostCompositeTarget;

	Check(!CanRecordOverlay(CommandListHookState::Uninitialized),
		"overlay recording waits for command-list hook installation");
	Check(!CanRecordOverlay(CommandListHookState::Installing),
		"overlay recording is blocked while command-list hooks are partially installed");
	Check(CanRecordOverlay(CommandListHookState::Ready),
		"overlay recording starts only after command-list hooks pass self-test");
	Check(!CanRecordOverlay(CommandListHookState::Failed),
		"overlay recording remains disabled after command-list hook failure");

	Check(CanChainForeignExecute(0x140000000),
		"foreign execute hooks are chained by default");
	Check(!CanChainForeignExecute(0),
		"a null slot has no engine pass to chain and is refused");
	Check(PostCompositeTargetMatchesOutputAspect(3440, 1440, 3440, 1440),
		"an output-sized post-composite target retains the validated path");
	Check(PostCompositeTargetMatchesOutputAspect(2580, 1080, 3440, 1440),
		"a render-scaled target with the output aspect retains the validated path");
	Check(PostCompositeTargetMatchesOutputAspect(2293, 960, 3440, 1440),
		"one-pixel render-scale rounding remains aspect-compatible");
	Check(PostCompositeTargetMatchesOutputAspect(2580, 1081, 3440, 1440),
		"a one-pixel target rounding difference remains aspect-compatible");
	Check(!PostCompositeTargetMatchesOutputAspect(2580, 1082, 3440, 1440),
		"a target outside the rounding envelope is rejected");
	Check(!PostCompositeTargetMatchesOutputAspect(2560, 1440, 3440, 1440),
		"a true 16:9 target is rejected for an ultrawide output");
	Check(!PostCompositeTargetMatchesOutputAspect(512, 512, 3440, 1440),
		"an auxiliary square target is rejected for an ultrawide output");
	Check(PostCompositeTargetMatchesOutputAspect(1280, 720, 1920, 1080),
		"a scaled 16:9 target is accepted for a 16:9 output");
	Check(PostCompositeTargetMatchesOutputAspect(512, 512, 0, 0),
		"unknown output dimensions do not reject a target speculatively");
	Check(PostCompositeTargetMatchesOutputAspect(512, 512, 3440, 0),
		"partially observed output dimensions remain fail-open");
	Check(PostCompositeTargetMatchesOutputAspect(512, 512, 0, 1440),
		"either missing output dimension keeps target validation fail-open");
	Check(!PostCompositeTargetMatchesOutputAspect(0, 512, 3440, 1440),
		"an invalid target never passes geometry validation");
	auto postTarget = SelectPostCompositeTarget(true, {});
	Check(!postTarget.supported,
		"vanilla uses ScaleformEnd so generated and rendered frames share the overlay");
	postTarget = SelectPostCompositeTarget(false, "LuMa.DlL");
	Check(postTarget.supported && postTarget.format == PostCompositeTargetFormat::Rgba16Float,
		"Luma post-composite selection accepts its RGBA16F UI target case-insensitively");
	postTarget = SelectPostCompositeTarget(false, "UnknownOverlay.dll");
	Check(!postTarget.supported,
		"an unproven foreign composite owner falls back to ScaleformEnd");

	ScaleformHandoffWindow handoff;
	Check(!handoff.TrackingHeaps() && !handoff.HandoffArmed(),
		"heap tracking starts outside the Scaleform composite pass");
	handoff.End();
	Check(!handoff.HandoffArmed(),
		"an End without a preceding Begin cannot arm a handoff");
	handoff.Begin();
	Check(handoff.TrackingHeaps() && !handoff.HandoffArmed(),
		"Begin opens heap tracking without scanning unrelated barriers");
	handoff.End();
	Check(handoff.HandoffArmed() && handoff.ConsumeAndReportFirstCandidate() &&
			handoff.TrackingHeaps(),
		"the first post-composite target keeps tracking open for the FG target");
	for (int i = 0; i < 4; ++i) {
		handoff.OnBarrierCall();
	}
	Check(handoff.HandoffArmed(),
		"the existing four-call handoff grace remains intact");
	handoff.OnBarrierCall();
	Check(!handoff.HandoffArmed() && !handoff.TrackingHeaps(),
		"a stale handoff closes heap tracking after the grace window");

	handoff.Begin();
	handoff.End();
	Check(handoff.ConsumeAndReportFirstCandidate(),
		"the first target in a new region is classified as first");
	Check(!handoff.ConsumeAndReportFirstCandidate() && !handoff.HandoffArmed() &&
			!handoff.TrackingHeaps(),
		"the second target closes the composite-to-handoff heap window");
	handoff.Begin();
	handoff.Cancel();
	Check(!handoff.TrackingHeaps(),
		"the next Scaleform Begin closes an unfinished post-composite window");

	FrameGenerationTargetPolicy targets;
	auto decision = targets.Observe(false, true);
	Check(!decision.draw && !decision.frameGeneration,
		"the first unclassified composite target waits for a complete region");

	decision = targets.Observe(false, true);
	Check(decision.draw && decision.firstDrawInRegion && !decision.frameGeneration,
		"a classified non-FG region draws its composite target");

	decision = targets.Observe(false, true);
	Check(decision.draw && decision.firstDrawInRegion && !decision.frameGeneration,
		"the FG activation region preserves its first composite draw");
	decision = targets.Observe(true, false);
	Check(decision.draw && decision.firstDrawInRegion && decision.frameGeneration,
		"the FG UI target becomes the effective first draw when FG appears");

	decision = targets.Observe(false, true);
	Check(!decision.draw && decision.frameGeneration,
		"steady FG suppresses the opaque interpolation target");
	decision = targets.Observe(true, false);
	Check(decision.draw && decision.firstDrawInRegion && decision.frameGeneration,
		"steady FG draws only the transparent UI target");

	decision = targets.Observe(false, true);
	Check(!decision.draw && decision.frameGeneration,
		"the first region after FG disappears remains fail-closed");
	decision = targets.Observe(false, true);
	Check(decision.draw && decision.firstDrawInRegion && !decision.frameGeneration &&
			!targets.FrameGenerationActive(),
		"the following non-FG region restores the composite target");

	if (failures == 0) {
		std::cout << "ui_pass_policy_tests: ok\n";
	}
	return failures;
}
