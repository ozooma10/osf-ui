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
