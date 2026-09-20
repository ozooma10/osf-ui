#include "Runtime/RuntimeHealthCoordinator.h"

#include "Runtime/Runtime.h"

namespace OSFUI
{
	void RuntimeHealthCoordinator::Pump()
	{
		_runtime._osfSettings.SyncDiagnostics(_runtime._healthRegistry.ActiveIssues());
	}

	void RuntimeHealthCoordinator::OnViewGreeted(std::string_view a_viewId)
	{
		_viewProtocolFaultCounts.erase(std::string(a_viewId));
		_runtime._healthRegistry.Resolve(std::format("view.protocol-misuse:{}", a_viewId));
	}

	void RuntimeHealthCoordinator::OnRendererHealth(
		const WebView2HostWebRenderer::HealthEvent& a_event)
	{
		auto& runtime = _runtime;
		_healthReconciler.ReportRendererHealth(runtime._healthRegistry, a_event.code,
			a_event.active, a_event.detail, runtime._renderer != nullptr);
	}

	void RuntimeHealthCoordinator::ReportViewLoad(std::string_view a_viewId, bool a_failed,
		std::string_view a_description, int a_errorCode, std::uint32_t a_attemptsLeft)
	{
		auto& runtime = _runtime;
		_healthReconciler.ReportViewLoad(runtime._healthRegistry, a_viewId, a_failed,
			a_description, a_errorCode, a_attemptsLeft);
	}

	void RuntimeHealthCoordinator::ReportProtocolFault(
		std::string_view a_viewId, std::string_view a_code)
	{
		if (a_viewId.empty()) return;
		constexpr std::uint32_t kProtocolFaultThreshold = 10;
		const auto count = ++_viewProtocolFaultCounts[std::string(a_viewId)];
		if (count == kProtocolFaultThreshold) {
			_healthReconciler.ReportProtocolMisuse(_runtime._healthRegistry,
				a_viewId, a_code, count);
		}
	}
}
