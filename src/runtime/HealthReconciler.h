#pragma once

#include "runtime/HealthRegistry.h"

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
		// Which compat condition this is. All `compat` producers must flow
		// through ONE SyncCompatibility call: the sweep below resolves by
		// SOURCE, so two independent producers would each resolve the other's
		// issues on every pass.
		std::string code{ "compat.needs-newer-osfui" };
		HealthRegistry::Severity severity{ HealthRegistry::Severity::Warning };
		std::string removalVersion;
		std::string declaration{ "targetVersion" };
	};

	// Testable state reconciliation behind RuntimeHealthCoordinator. Runtime gathers
	// engine/store facts; this class owns issue identity, severity, and lifecycle.
	class HealthReconciler final
	{
	public:
		void SyncSettings(HealthRegistry& a_healthRegistry,
			std::span<const SettingsLoadIssue> a_errors, double a_now);
		void SyncCompatibility(HealthRegistry& a_healthRegistry,
			std::span<const CompatibilityTarget> a_targets,
			std::string_view a_installedVersion, double a_now);
		void ReportViewLoad(HealthRegistry& a_healthRegistry,
			std::string_view a_viewId, bool a_failed,
			std::string_view a_description, int a_errorCode,
			std::uint32_t a_attemptsLeft, double a_now);

	private:
		std::string _settingsSignature;
		std::string _compatSignature;
	};
}
