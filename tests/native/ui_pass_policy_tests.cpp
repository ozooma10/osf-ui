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
	using OSFUI::UiPass::detail::ExecuteSlotKind;

	Check(CanChainForeignExecute(ExecuteSlotKind::Composite, "Luma.dll"),
		"Luma may own ScaleformComposite");
	Check(CanChainForeignExecute(ExecuteSlotKind::Composite, "LUMA.DLL"),
		"Windows module matching is case-insensitive");
	Check(!CanChainForeignExecute(ExecuteSlotKind::Begin, "Luma.dll"),
		"Luma may not replace ScaleformBegin");
	Check(!CanChainForeignExecute(ExecuteSlotKind::End, "Luma.dll"),
		"Luma may not replace ScaleformEnd");
	Check(!CanChainForeignExecute(ExecuteSlotKind::Composite, "OtherOverlay.dll"),
		"unknown Composite hooks remain fail-closed");
	Check(!CanChainForeignExecute(ExecuteSlotKind::Composite, "Luma.dll.backup"),
		"module names must match exactly");
	Check(!CanChainForeignExecute(ExecuteSlotKind::Composite, ""),
		"unattributed hooks remain fail-closed");

	if (failures == 0) {
		std::cout << "ui_pass_policy_tests: ok\n";
	}
	return failures;
}
