#pragma once

#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json.hpp>

#include "vendor/OSFSettings.h"
#include "vendor/OSFSettings_Diagnostics.h"

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
		// Known codes get their user-facing text and severity from Describe; a_message titles anything else.
		// Technical context stays in the native log, never in Settings issue text.
		void ReportFailure(std::string_view a_id, std::string_view a_code, std::string_view a_message,
			const nlohmann::json& a_context = nlohmann::json::object());
		void ClearFailure(std::string_view a_id);
		// Retries reports and clears the SDK rejected earlier. Called once per runtime tick.
		void RetryDiagnostics();
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
		static IssueText Describe(std::string_view a_code, std::string_view a_message, const nlohmann::json& a_context);
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
		// What OSF UI wants shown versus what Settings accepted. Rejected reports/clears retry on the next tick.
		std::unordered_map<std::string, IssueText> _desired;
		std::unordered_map<std::string, IssueText> _reported;
		std::unordered_set<std::string> _failedOperations;
	};
}
