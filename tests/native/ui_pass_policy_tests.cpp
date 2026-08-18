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

	if (failures == 0) {
		std::cout << "ui_pass_policy_tests: ok\n";
	}
	return failures;
}
