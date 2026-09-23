#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

#include "vendor/OSFSettings.h"
#include "vendor/OSFSettings_Diagnostics.h"
#include "Diagnostics/HealthRegistry.h"

namespace OSFUI
{
	struct ViewManifest;
	// All calls belong to the runtime thread. SDK interfaces live for the process lifetime.
	class OSFSettingsClient final
	{
	public:
		bool Initialize();
		void RegisterLaunchers(std::span<const ViewManifest> a_views);
		[[nodiscard]] bool Available() const { return _available; }
		[[nodiscard]] bool DeveloperMode() const { return _developerMode; }
		[[nodiscard]] bool HighRefreshCapture() const { return _highRefreshCapture; }
		void SyncDiagnostics(std::span<const HealthRegistry::IssueSpec> a_issues);
		void ReportFailure(std::string_view a_id, std::string_view a_code, std::string_view a_message,
			const nlohmann::json& a_context = nlohmann::json::object());
		void ClearFailure(std::string_view a_id);
		[[nodiscard]] bool AcquireInputSuppression();
		bool ReleaseInputSuppression();

	private:
		struct IssueText
		{
			OSFSettings::API::Diagnostics::Severity severity{ OSFSettings::API::Diagnostics::Severity::Error };
			std::string title;
			std::string impact;
			std::string nextSteps;
			bool operator==(const IssueText&) const = default;
		};
		static IssueText Describe(const HealthRegistry::IssueSpec& a_issue);
		void ReadStartupBool(const char* a_key, bool& a_value);
		void Report(std::string_view a_id, const IssueText& a_issue);
		bool Clear(std::string_view a_id);
		bool Check(OSFSettings::API::Status a_status, std::string a_operation);

		OSFSettings::API::Client _settings;
		OSFSettings::API::Diagnostics::Client _diagnostics;
		bool _available{};
		bool _developerMode{};
		bool _highRefreshCapture{};
		OSFSettings::API::HotkeyBlock _hotkeyBlock{};
		std::unordered_map<std::string, IssueText> _directFailures;
		// Cache only accepted reports. Failed reports/clears are retried by the next runtime tick.
		std::unordered_map<std::string, IssueText> _reported;
		std::unordered_set<std::string> _failedOperations;
	};
}
