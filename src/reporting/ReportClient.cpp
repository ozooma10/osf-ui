#include "reporting/ReportClient.h"

#include "core/Paths.h"
#include "core/Version.h"
#include "platform/WindowsPlatform.h"
#include "reporting/ReporterCore.h"

#include <array>

namespace OSFUI::Reporting
{
	namespace
	{
		constexpr std::size_t kMaxLogBytes = 384 * 1024;

		[[nodiscard]] std::string RedactLog(std::string a_text)
		{
			const std::array redactions{
				Redaction{ Platform::GetDocumentsPath().string(), "<Documents>" },
				Redaction{ Paths::PluginDir().string(), "<PluginDir>" },
			};
			return Redact(std::move(a_text), redactions);
		}

		void AddLog(nlohmann::json& a_logs, const std::filesystem::path& a_path,
			std::string_view a_name)
		{
			bool truncated = false;
			auto content = ReadTail(a_path, kMaxLogBytes, truncated);
			if (content.empty()) return;
			a_logs.push_back({
				{ "name", a_name },
				{ "content", RedactLog(std::move(content)) },
				{ "truncated", truncated },
			});
		}
	}

	SubmissionResult Submit(std::string_view a_endpoint,
		const nlohmann::json& a_diagnostics, std::string_view a_title,
		std::string_view a_description, std::string_view a_reproduction)
	{
		nlohmann::json logs = nlohmann::json::array();
		const auto logDir = Paths::LogDir();
		if (!logDir.empty()) {
			AddLog(logs, logDir / "OSF UI.log", "OSF UI.log");
			AddLog(logs, logDir / "OSF UI.webview2-host.log", "OSF UI.webview2-host.log");
		}
		const auto payload = [&](std::string_view a_clientId, std::string_view a_token) {
			return nlohmann::json{
				{ "schemaVersion", 1 }, { "clientId", a_clientId },
				{ "installationToken", a_token }, { "kind", "manual" },
				{ "target", "osf-ui" },
				{ "title", Bounded(a_title, 120) },
				{ "description", Bounded(a_description, 6000) },
				{ "reproduction", Bounded(a_reproduction, 4000) },
				{ "pluginVersion", kPluginVersion }, { "diagnostics", a_diagnostics },
				{ "logs", logs },
			};
		};
		const auto submission = SubmitAuthenticated(a_endpoint,
			ReporterFolder(Platform::GetDocumentsPath()), payload, PostJson);
		if (!submission.errorCode.empty()) {
			return { .code = submission.errorCode, .message = submission.errorMessage };
		}
		if (submission.response.status < 200 || submission.response.status >= 300 ||
			!submission.body.value("ok", false)) {
			return {
				.code = submission.body.value("code", "service-failed"),
				.message = "report service refused the submission",
			};
		}
		return {
			.ok = true,
			.reportId = submission.body.value("reportId", ""),
			.issueUrl = submission.body.value("issueUrl", ""),
			.issueNumber = submission.body.value("issueNumber", 0ull),
		};
	}
}
