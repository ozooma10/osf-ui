#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace OSFUI::Reporting
{
	struct HttpResponse
	{
		bool          transportOk{ false };
		std::uint32_t status{ 0 };
		std::string   body;
		std::string   error;
	};

	using PostFunction = std::function<HttpResponse(std::string_view, std::string_view)>;
	using PayloadFactory = std::function<nlohmann::json(std::string_view, std::string_view)>;

	struct AuthenticatedResponse
	{
		HttpResponse   response;
		nlohmann::json body;
		std::string    errorCode;
		std::string    errorMessage;
	};

	struct Redaction
	{
		std::string value;
		std::string replacement;
	};

	[[nodiscard]] std::string DumpSafe(const nlohmann::json& a_value);
	[[nodiscard]] std::string Bounded(std::string_view a_text, std::size_t a_maxBytes);
	[[nodiscard]] std::string ReadTail(const std::filesystem::path& a_path,
		std::size_t a_maxBytes, bool& a_truncated);
	[[nodiscard]] std::string Redact(std::string a_text,
		std::span<const Redaction> a_redactions);

	[[nodiscard]] std::filesystem::path ReporterFolder(
		const std::filesystem::path& a_documentsFolder);
	[[nodiscard]] std::string ReporterClientId(const std::filesystem::path& a_folder);
	[[nodiscard]] bool IsValidInstallationToken(std::string_view a_token);
	[[nodiscard]] std::string InstallationEndpoint(std::string_view a_reportEndpoint);

	// Register or reuse the installation ticket, submit the payload, and renew
	// once on the service's invalid-installation response. The payload factory is
	// called again after renewal so the replacement token reaches the retry.
	[[nodiscard]] AuthenticatedResponse SubmitAuthenticated(
		std::string_view a_endpoint,
		const std::filesystem::path& a_reporterFolder,
		const PayloadFactory& a_payload,
		const PostFunction& a_post);

	// Shared WinHTTP transport for the DLL and out-of-process host.
	[[nodiscard]] HttpResponse PostJson(std::string_view a_url, std::string_view a_body);
}
