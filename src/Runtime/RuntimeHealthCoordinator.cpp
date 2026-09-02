#include "Runtime/RuntimeHealthCoordinator.h"

#include "Composite/UiPass.h"
#include "Core/Version.h"
#include "Runtime/Runtime.h"

namespace OSFUI
{
	namespace { constexpr double kPollSeconds{ 2.0 }; }

	void RuntimeHealthCoordinator::Pump()
	{
		auto& runtime = _runtime;
		if (runtime._uptime >= _nextPoll) {
			_nextPoll = runtime._uptime + kPollSeconds;
			UpdateSystemInfo();
		}
		runtime._osfSettings.SyncDiagnostics(runtime._healthRegistry.Snapshot());
	}

	void RuntimeHealthCoordinator::UpdateSystemInfo()
	{
		auto& runtime = _runtime;
		runtime._healthRegistry.SetSystemInfo(nlohmann::json{
			{ "version", kOsfuiReleaseVersion },
			{ "bridgeVersion", kBridgeProtocolVersion },
			{ "renderer", runtime._renderer ? "webview2" : "deferred" },
			{ "compositor", runtime._compositor ? "d3d12" : "deferred" },
			{ "drawPath", runtime.OverlayCanDraw() ? "ui-pass" : "deferred" },
			{ "frameGeneration", UiPass::FrameGenerationActive() },
			{ "devMode", runtime._developerMode },
			{ "highRefreshCapture", runtime._highRefreshCapture },
		});
	}

	void RuntimeHealthCoordinator::OnRendererHealth(
		const WebView2HostWebRenderer::HealthEvent& a_event)
	{
		auto& runtime = _runtime;
		_healthReconciler.ReportRendererHealth(runtime._healthRegistry, a_event.code,
			a_event.active, a_event.detail, runtime._renderer != nullptr, runtime._uptime);
	}

	void RuntimeHealthCoordinator::ReportViewLoad(std::string_view a_viewId, bool a_failed,
		std::string_view a_description, int a_errorCode, std::uint32_t a_attemptsLeft)
	{
		auto& runtime = _runtime;
		_healthReconciler.ReportViewLoad(runtime._healthRegistry, a_viewId, a_failed,
			a_description, a_errorCode, a_attemptsLeft, runtime._uptime);
	}

	void RuntimeHealthCoordinator::ReportProtocolFault(
		std::string_view a_viewId, std::string_view a_code)
	{
		if (a_viewId.empty()) return;
		constexpr std::uint32_t kProtocolFaultThreshold = 10;
		const auto count = ++_viewProtocolFaultCounts[std::string(a_viewId)];
		if (count == kProtocolFaultThreshold) {
			_healthReconciler.ReportProtocolMisuse(_runtime._healthRegistry,
				a_viewId, a_code, count, _runtime._uptime);
		}
	}
}
