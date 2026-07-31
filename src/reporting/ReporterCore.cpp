#include "reporting/ReporterCore.h"

#include "core/StringUtil.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <random>
#include <regex>

namespace OSFUI::Reporting
{
	namespace
	{
		[[nodiscard]] bool IsValidClientId(std::string_view a_id)
		{
			return a_id.size() >= 16 && a_id.size() <= 80 &&
				std::ranges::all_of(a_id, [](unsigned char c) {
					return std::isalnum(c) || c == '_' || c == '-';
				});
		}

		void ReplaceAllInsensitive(std::string& a_text, std::string_view a_needle,
			std::string_view a_replacement)
		{
			if (a_needle.empty()) return;
			auto lowerText = StringUtil::ToLowerAscii(a_text);
			const auto lowerNeedle = StringUtil::ToLowerAscii(a_needle);
			std::size_t at = 0;
			while ((at = lowerText.find(lowerNeedle, at)) != std::string::npos) {
				a_text.replace(at, a_needle.size(), a_replacement);
				lowerText.replace(at, a_needle.size(), a_replacement);
				at += a_replacement.size();
			}
		}

		void ReplacePath(std::string& a_text, std::string a_path,
			std::string_view a_replacement)
		{
			if (a_path.empty()) return;
			ReplaceAllInsensitive(a_text, a_path, a_replacement);
			std::ranges::replace(a_path, '\\', '/');
			ReplaceAllInsensitive(a_text, a_path, a_replacement);
		}

		[[nodiscard]] std::string InstallationToken(std::string_view a_endpoint,
			std::string_view a_clientId, const std::filesystem::path& a_folder,
			bool a_forceRenew, const PostFunction& a_post)
		{
			const auto path = a_folder / "reporter-ticket.txt";
			std::string token;
			if (!a_forceRenew && !a_folder.empty()) {
				std::ifstream existing(path);
				if (existing >> token && IsValidInstallationToken(token)) return token;
			}

			const auto registration = InstallationEndpoint(a_endpoint);
			if (registration.empty()) return {};
			const auto response = a_post(registration,
				DumpSafe(nlohmann::json{ { "clientId", a_clientId } }));
			if (!response.transportOk || response.status < 200 || response.status >= 300) return {};
			const auto parsed = nlohmann::json::parse(response.body, nullptr, false);
			if (parsed.is_discarded() || !parsed.is_object()) return {};
			token = parsed.value("installationToken", "");
			if (!IsValidInstallationToken(token)) return {};
			if (!a_folder.empty()) {
				std::error_code ec;
				std::filesystem::create_directories(a_folder, ec);
				std::ofstream out(path, std::ios::trunc);
				if (out) out << token;
			}
			return token;
		}
	}

	std::string DumpSafe(const nlohmann::json& a_value)
	{
		return a_value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
	}

	std::string Bounded(std::string_view a_text, std::size_t a_maxBytes)
	{
		return std::string{ a_text.substr(0, StringUtil::Utf8TruncateLen(a_text, a_maxBytes)) };
	}

	std::string ReadTail(const std::filesystem::path& a_path,
		std::size_t a_maxBytes, bool& a_truncated)
	{
		a_truncated = false;
		std::ifstream file(a_path, std::ios::binary | std::ios::ate);
		if (!file || a_maxBytes == 0) return {};
		const auto end = file.tellg();
		if (end <= 0) return {};
		const auto size = static_cast<std::size_t>(end);
		const auto keep = (std::min)(size, a_maxBytes);
		a_truncated = keep < size;
		file.seekg(static_cast<std::streamoff>(size - keep));
		std::string text(keep, '\0');
		file.read(text.data(), static_cast<std::streamsize>(keep));
		text.resize(static_cast<std::size_t>(file.gcount()));
		if (a_truncated) {
			const auto trimmed = StringUtil::SkipLeadingUtf8Continuations(text);
			text.erase(0, text.size() - trimmed.size());
		}
		return text;
	}

	std::string Redact(std::string a_text, std::span<const Redaction> a_redactions)
	{
		for (const auto& item : a_redactions) {
			ReplacePath(a_text, item.value, item.replacement);
		}
		static const std::regex profile(
			R"(([A-Za-z]:[\\/](?:Users|Documents and Settings)[\\/])[^\\/\r\n"']+)",
			std::regex::icase);
		return std::regex_replace(a_text, profile, "$1<user>");
	}

	std::filesystem::path ReporterFolder(const std::filesystem::path& a_documentsFolder)
	{
		return a_documentsFolder.empty() ? std::filesystem::path{} :
			a_documentsFolder / "My Games" / "Starfield" / "OSFUI";
	}

	std::string ReporterClientId(const std::filesystem::path& a_folder)
	{
		const auto path = a_folder / "reporter-id.txt";
		std::string id;
		if (!a_folder.empty()) {
			std::ifstream existing(path);
			if (existing >> id && IsValidClientId(id)) return id;
		}

		std::random_device random;
		static constexpr char hex[] = "0123456789abcdef";
		id = "client_";
		for (int i = 0; i < 32; ++i) id += hex[random() & 0xF];
		if (!a_folder.empty()) {
			std::error_code ec;
			std::filesystem::create_directories(a_folder, ec);
			std::ofstream out(path, std::ios::trunc);
			if (out) out << id;
		}
		return id;
	}

	bool IsValidInstallationToken(std::string_view a_token)
	{
		return a_token.size() >= 40 && a_token.size() <= 512 &&
			std::ranges::all_of(a_token, [](unsigned char c) {
				return std::isalnum(c) || c == '_' || c == '-' || c == '.';
			});
	}

	std::string InstallationEndpoint(std::string_view a_reportEndpoint)
	{
		std::string endpoint(a_reportEndpoint);
		constexpr std::string_view suffix = "/v1/reports";
		if (!endpoint.ends_with(suffix)) return {};
		endpoint.resize(endpoint.size() - suffix.size());
		return endpoint + "/v1/installations";
	}

	AuthenticatedResponse SubmitAuthenticated(std::string_view a_endpoint,
		const std::filesystem::path& a_reporterFolder,
		const PayloadFactory& a_payload, const PostFunction& a_post)
	{
		AuthenticatedResponse out;
		const auto clientId = ReporterClientId(a_reporterFolder);
		auto token = InstallationToken(a_endpoint, clientId, a_reporterFolder, false, a_post);
		if (token.empty()) {
			out.errorCode = "registration-failed";
			out.errorMessage = "could not register this installation";
			return out;
		}

		const auto send = [&] {
			out.response = a_post(a_endpoint, DumpSafe(a_payload(clientId, token)));
			if (out.response.transportOk) {
				out.body = nlohmann::json::parse(out.response.body, nullptr, false);
			}
		};
		send();
		if (!out.response.transportOk) {
			out.errorCode = "network-failed";
			out.errorMessage = out.response.error;
			return out;
		}
		if (out.response.status == 401 && out.body.is_object() &&
			out.body.value("code", "") == "invalid-installation") {
			token = InstallationToken(a_endpoint, clientId, a_reporterFolder, true, a_post);
			if (token.empty()) {
				out.errorCode = "registration-failed";
				out.errorMessage = "could not renew this installation";
				return out;
			}
			send();
			if (!out.response.transportOk) {
				out.errorCode = "network-failed";
				out.errorMessage = out.response.error;
				return out;
			}
		}
		if (out.body.is_discarded() || !out.body.is_object()) {
			out.errorCode = "invalid-response";
			out.errorMessage = "report service returned an invalid response";
		}
		return out;
	}
}
