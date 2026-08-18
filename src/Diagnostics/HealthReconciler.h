#pragma once

#include "Diagnostics/HealthRegistry.h"

#include <span>

namespace OSFUI
{
	struct SettingsLoadIssue
	{
		std::string kind;
		std::string file;
		std::string subject;
		std::string message;
		nlohmann::json context{ nlohmann::json::object() };
	};

	struct CompatibilityTarget
	{
		std::string id;
		std::string kind;
		std::string targetVersion;
		std::string code{ "compat.needs-newer-osfui" };
		HealthRegistry::Severity severity{ HealthRegistry::Severity::Warning };
		std::string declaration{ "targetVersion" };
	};

	// Testable state reconciliation behind RuntimeHealthCoordinator. Runtime gathers engine/store facts; this class owns OSF UI issue identity, severity, and lifecycle.
	class HealthReconciler final
	{
	public:
		void SyncSettings(HealthRegistry& a_healthRegistry, std::span<const SettingsLoadIssue> a_errors, double a_now);
		void SyncCompatibility(HealthRegistry& a_healthRegistry, std::span<const CompatibilityTarget> a_targets, std::string_view a_installedVersion, double a_now);
		void ReportViewLoad(HealthRegistry& a_healthRegistry, std::string_view a_viewId, bool a_failed, std::string_view a_description, int a_errorCode, std::uint32_t a_attemptsLeft, double a_now);
		void SyncControlMap(HealthRegistry& a_healthRegistry, bool a_available, std::string_view a_gameVersion, std::string_view a_reason, double a_now);
		void ReportRendererHealth(HealthRegistry& a_healthRegistry, std::string_view a_code, bool a_active, std::string_view a_detail, bool a_rendererAvailable, double a_now);
		void ReportProtocolMisuse(HealthRegistry& a_healthRegistry, std::string_view a_viewId, std::string_view a_code, std::uint32_t a_count, double a_now);
	};
}
