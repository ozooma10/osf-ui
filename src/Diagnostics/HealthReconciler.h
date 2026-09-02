#pragma once

#include "Diagnostics/HealthRegistry.h"

namespace OSFUI
{
	// Testable state reconciliation behind RuntimeHealthCoordinator. Runtime gathers engine/store facts; this class owns OSF UI issue identity, severity, and lifecycle.
	class HealthReconciler final
	{
	public:
		void ReportViewLoad(HealthRegistry& a_healthRegistry, std::string_view a_viewId, bool a_failed, std::string_view a_description, int a_errorCode, std::uint32_t a_attemptsLeft, double a_now);
		void ReportRendererHealth(HealthRegistry& a_healthRegistry, std::string_view a_code, bool a_active, std::string_view a_detail, bool a_rendererAvailable, double a_now);
		void ReportProtocolMisuse(HealthRegistry& a_healthRegistry, std::string_view a_viewId, std::string_view a_code, std::uint32_t a_count, double a_now);
	};
}
