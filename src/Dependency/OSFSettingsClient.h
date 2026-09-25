#pragma once

#include <span>
#include <string>
#include <unordered_set>
#include <nlohmann/json.hpp>

#include "vendor/OSFSettings.h"
#include "vendor/OSFSettings_Diagnostics.h"

namespace OSFUI
{
	struct ViewManifest;
	class OSFSettingsClient final
	{
	public:
		bool Initialize();
		void RegisterLaunchers(std::span<const ViewManifest> a_views);
		[[nodiscard]] bool Available() const { return m_available; }
		[[nodiscard]] bool DeveloperMode() const { return m_developerMode; }
		[[nodiscard]] bool HighRefreshCapture() const { return m_highRefreshCapture; }
		// Game language code from OSF Settings, lower-cased ("en", "de", "ptbr"). Empty until known: the game's translations load after kPostPostLoad, so each call retries until Settings reports it, then it is cached.
		[[nodiscard]] std::string Language();
		// Known codes get their user-facing text and severity from Describe; a_message titles anything else.
		void ReportFailure(std::string_view a_id, std::string_view a_code, std::string_view a_message, const nlohmann::json& a_context = nlohmann::json::object());
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
		};
		static IssueText Describe(std::string_view a_code, std::string_view a_message, const nlohmann::json& a_context);
		void ReadStartupBool(const char* a_key, bool& a_value);
		void Report(std::string_view a_id, const IssueText& a_issue);
		bool Check(OSFSettings::API::Status a_status, std::string a_operation);

		OSFSettings::API::Client m_settings;
		OSFSettings::API::Diagnostics::Client m_diagnostics;
		bool m_available{};
		bool m_developerMode{};
		bool m_highRefreshCapture{};
		std::string m_language;
		OSFSettings::API::HotkeyBlock m_hotkeyBlock{};
		std::unordered_set<std::string> m_failedOperations;
	};
}
