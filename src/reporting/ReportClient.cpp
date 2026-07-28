#include "reporting/ReportClient.h"

#include <cctype>
#include <fstream>
#include <random>
#include <regex>

#include "core/Paths.h"
#include "core/Version.h"
#include "platform/WindowsPlatform.h"

namespace OSFUI::Reporting
{
	namespace
	{
		constexpr std::size_t kMaxLogBytes = 384 * 1024;

		std::filesystem::path ReporterFolder()
		{
			const auto documents = Platform::GetDocumentsPath();
			return documents.empty() ? std::filesystem::path{} :
				documents / "My Games" / "Starfield" / "OSFUI";
		}

		std::string ReadTail(const std::filesystem::path& a_path, bool& a_truncated)
		{
			std::ifstream file(a_path, std::ios::binary | std::ios::ate);
			if (!file) return {};
			const auto end = file.tellg();
			if (end <= 0) return {};
			const auto size = static_cast<std::size_t>(end);
			const auto keep = (std::min)(size, kMaxLogBytes);
			a_truncated = keep < size;
			file.seekg(static_cast<std::streamoff>(size - keep));
			std::string text(keep, '\0');
			file.read(text.data(), static_cast<std::streamsize>(keep));
			text.resize(static_cast<std::size_t>(file.gcount()));
			return text;
		}

		void ReplaceAllInsensitive(std::string& a_text, std::string_view a_needle,
			std::string_view a_replacement)
		{
			if (a_needle.empty()) return;
			std::string lowerText = a_text;
			std::string lowerNeedle(a_needle);
			std::ranges::transform(lowerText, lowerText.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			std::ranges::transform(lowerNeedle, lowerNeedle.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			std::size_t at = 0;
			while ((at = lowerText.find(lowerNeedle, at)) != std::string::npos) {
				a_text.replace(at, a_needle.size(), a_replacement);
				lowerText.replace(at, a_needle.size(), a_replacement);
				at += a_replacement.size();
			}
		}

		std::string Redact(std::string text)
		{
			ReplaceAllInsensitive(text, Platform::GetDocumentsPath().string(), "<Documents>");
			ReplaceAllInsensitive(text, Paths::PluginDir().string(), "<PluginDir>");
			// Backstop for profile references outside the Documents tree.
			static const std::regex profile(
				R"(([A-Za-z]:[\\/](?:Users|Documents and Settings)[\\/])[^\\/\r\n"']+)",
				std::regex::icase);
			return std::regex_replace(text, profile, "$1<user>");
		}

		std::string ClientId()
		{
			const auto folder = ReporterFolder();
			const auto path = folder / "reporter-id.txt";
			std::ifstream existing(path);
			std::string id;
			if (existing >> id && id.size() >= 16 && id.size() <= 80 &&
				std::ranges::all_of(id, [](unsigned char c) {
					return std::isalnum(c) || c == '_' || c == '-';
				})) {
				return id;
			}
			std::random_device random;
			static constexpr char hex[] = "0123456789abcdef";
			id = "client_";
			for (int i = 0; i < 32; ++i) id += hex[random() & 0xF];
			if (!folder.empty()) {
				std::error_code ec;
				std::filesystem::create_directories(folder, ec);
				std::ofstream out(path, std::ios::trunc);
				if (out) out << id;
			}
			return id;
		}

		std::string InstallationEndpoint(std::string_view a_reportEndpoint)
		{
			std::string endpoint(a_reportEndpoint);
			constexpr std::string_view suffix = "/v1/reports";
			if (!endpoint.ends_with(suffix)) return {};
			endpoint.resize(endpoint.size() - suffix.size());
			return endpoint + "/v1/installations";
		}

		std::string InstallationToken(std::string_view a_endpoint,
			std::string_view a_clientId, bool a_forceRenew)
		{
			const auto folder = ReporterFolder();
			const auto path = folder / "reporter-ticket.txt";
			std::string token;
			if (!a_forceRenew) {
				std::ifstream existing(path);
				if (existing >> token && token.size() >= 40 && token.size() <= 512 &&
					std::ranges::all_of(token, [](unsigned char c) {
						return std::isalnum(c) || c == '_' || c == '-' || c == '.';
					})) {
					return token;
				}
			}
			const auto registration = InstallationEndpoint(a_endpoint);
			if (registration.empty()) return {};
			const auto response = Platform::PostJson(registration,
				nlohmann::json{ { "clientId", a_clientId } }.dump());
			if (!response.transportOk || response.status < 200 || response.status >= 300) return {};
			const auto parsed = nlohmann::json::parse(response.body, nullptr, false);
			if (parsed.is_discarded() || !parsed.is_object()) return {};
			token = parsed.value("installationToken", "");
			if (token.size() < 40 || token.size() > 512) return {};
			if (!folder.empty()) {
				std::error_code ec;
				std::filesystem::create_directories(folder, ec);
				std::ofstream out(path, std::ios::trunc);
				if (out) out << token;
			}
			return token;
		}

		void AddLog(nlohmann::json& a_logs, const std::filesystem::path& a_path,
			std::string_view a_name)
		{
			bool truncated = false;
			auto content = ReadTail(a_path, truncated);
			if (content.empty()) return;
			a_logs.push_back({
				{ "name", a_name },
				{ "content", Redact(std::move(content)) },
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
		const auto clientId = ClientId();
		auto installationToken = InstallationToken(a_endpoint, clientId, false);
		if (installationToken.empty()) {
			return { .code = "registration-failed", .message = "could not register this installation" };
		}
		const auto makePayload = [&] {
			return nlohmann::json{
				{ "schemaVersion", 1 }, { "clientId", clientId },
				{ "installationToken", installationToken }, { "kind", "manual" },
				{ "target", "osf-ui" },
				{ "title", std::string(a_title).substr(0, 120) },
				{ "description", std::string(a_description).substr(0, 6000) },
				{ "reproduction", std::string(a_reproduction).substr(0, 4000) },
				{ "pluginVersion", kPluginVersion }, { "diagnostics", a_diagnostics },
				{ "logs", logs },
			};
		};
		auto response = Platform::PostJson(a_endpoint, makePayload().dump());
		if (!response.transportOk) {
			return { .code = "network-failed", .message = response.error };
		}
		auto parsed = nlohmann::json::parse(response.body, nullptr, false);
		if (response.status == 401 && parsed.is_object() &&
			parsed.value("code", "") == "invalid-installation") {
			installationToken = InstallationToken(a_endpoint, clientId, true);
			if (installationToken.empty()) {
				return { .code = "registration-failed", .message = "could not renew this installation" };
			}
			response = Platform::PostJson(a_endpoint, makePayload().dump());
			if (!response.transportOk) return { .code = "network-failed", .message = response.error };
			parsed = nlohmann::json::parse(response.body, nullptr, false);
		}
		if (parsed.is_discarded() || !parsed.is_object()) {
			return { .code = "invalid-response", .message = "report service returned an invalid response" };
		}
		if (response.status < 200 || response.status >= 300 || !parsed.value("ok", false)) {
			return {
				.code = parsed.value("code", "service-failed"),
				.message = "report service refused the submission",
			};
		}
		return {
			.ok = true,
			.reportId = parsed.value("reportId", ""),
			.issueUrl = parsed.value("issueUrl", ""),
			.issueNumber = parsed.value("issueNumber", 0ull),
		};
	}
}
