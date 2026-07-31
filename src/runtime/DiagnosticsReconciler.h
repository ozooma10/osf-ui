#pragma once

#include "runtime/DiagnosticsModule.h"

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
	};

	// Testable state reconciliation behind RuntimeDiagnostics. Runtime gathers
	// engine/store facts; this class owns issue identity, severity, and lifecycle.
	class DiagnosticsReconciler final
	{
	public:
		void SyncSettings(DiagnosticsModule& a_diagnostics,
			std::span<const SettingsLoadIssue> a_errors, double a_now);
		void SyncCompatibility(DiagnosticsModule& a_diagnostics,
			std::span<const CompatibilityTarget> a_targets,
			std::string_view a_installedVersion, double a_now);
		void ReportViewLoad(DiagnosticsModule& a_diagnostics,
			std::string_view a_viewId, bool a_failed,
			std::string_view a_description, int a_errorCode,
			std::uint32_t a_attemptsLeft, double a_now);

	private:
		std::string _settingsSignature;
		std::string _compatSignature;
	};
}
