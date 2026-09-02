#pragma once

#include "Render/WebView2HostWebRenderer.h"
#include "Diagnostics/HealthReconciler.h"

#include <unordered_map>

namespace OSFUI
{
	class Runtime;

	class RuntimeHealthCoordinator final
	{
	public:
		explicit RuntimeHealthCoordinator(Runtime& a_runtime) : _runtime(a_runtime) {}

		void Pump();
		void OnRendererHealth(const WebView2HostWebRenderer::HealthEvent& a_event);
		void ReportViewLoad(std::string_view a_viewId, bool a_failed, std::string_view a_description, int a_errorCode, std::uint32_t a_attemptsLeft);
		void ReportProtocolFault(std::string_view a_viewId, std::string_view a_code);

	private:
		void UpdateSystemInfo();

		Runtime&         _runtime;
		HealthReconciler _healthReconciler;
		std::unordered_map<std::string, std::uint32_t> _viewProtocolFaultCounts;
		double _nextPoll{ 0.0 };
	};
}
