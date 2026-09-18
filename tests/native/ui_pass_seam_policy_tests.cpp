#include "composite/UiPassSeamPolicy.h"

#include <iostream>

#ifdef _WIN32
#include "composite/SeamTargetFormat.h"
#endif

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
	using OSFUI::UiPassSeam::detail::CanChainForeignExecute;
	using OSFUI::UiPassSeam::detail::ExecuteSlotKind;

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

#ifdef _WIN32
	using OSFUI::SeamTargetFormat::ResolveRtv;
	Check(ResolveRtv(DXGI_FORMAT_R8G8B8A8_TYPELESS) == DXGI_FORMAT_R8G8B8A8_UNORM,
		"stock typeless UI uses the RGBA8 pipeline");
	Check(ResolveRtv(DXGI_FORMAT_R8G8B8A8_UNORM) == DXGI_FORMAT_R8G8B8A8_UNORM,
		"stock typed UI retains RGBA8");
	Check(ResolveRtv(DXGI_FORMAT_R16G16B16A16_TYPELESS) == DXGI_FORMAT_R16G16B16A16_FLOAT,
		"typeless HDR UI uses the float pipeline");
	Check(ResolveRtv(DXGI_FORMAT_R16G16B16A16_FLOAT) == DXGI_FORMAT_R16G16B16A16_FLOAT,
		"Luma HDR UI retains its float format");
	Check(ResolveRtv(DXGI_FORMAT_R10G10B10A2_UNORM) == DXGI_FORMAT_UNKNOWN,
		"unproven HDR targets must not use either pipeline");
	Check(ResolveRtv(DXGI_FORMAT_UNKNOWN) == DXGI_FORMAT_UNKNOWN,
		"unknown targets remain unsupported");
#endif

	if (failures == 0) {
		std::cout << "ui_pass_seam_policy_tests: ok\n";
	}
	return failures;
}
