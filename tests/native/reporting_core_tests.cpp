#include "reporting/ReporterCore.h"

#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

using OSFUI::Reporting::HttpResponse;
using nlohmann::json;

namespace
{
	HttpResponse Reply(std::uint32_t a_status, const json& a_body)
	{
		return { .transportOk = true, .status = a_status, .body = a_body.dump() };
	}
}

int main()
{
	using namespace OSFUI::Reporting;
	constexpr std::string_view endpoint = "https://reports.example/v1/reports";
	assert(InstallationEndpoint(endpoint) == "https://reports.example/v1/installations");
	assert(InstallationEndpoint("https://reports.example/reports").empty());
	assert(!IsValidInstallationToken("short"));
	assert(IsValidInstallationToken(std::string(40, 'a')));
	assert(!IsValidInstallationToken(std::string(40, '/')));

	const std::array redactions{
		Redaction{ R"(C:\Private\Root)", "<Private>" },
	};
	const auto redacted = Redact(
		R"(C:\Private\Root\one C:/Private/Root/two C:\Users\Alice\file)", redactions);
	assert(!redacted.contains("Private\\Root"));
	assert(!redacted.contains("Private/Root"));
	assert(!redacted.contains("Alice"));
	assert(redacted.contains("<Private>"));
	assert(redacted.contains(R"(C:\Users\<user>\file)"));

	const std::filesystem::path folder = ".build/reporting-core";
	std::filesystem::remove_all(folder);
	std::filesystem::create_directories(folder);
	const auto tailPath = folder / "utf8.log";
	{
		std::ofstream out(tailPath, std::ios::binary);
		out << "aa\xE2\x82\xACtail";
	}
	bool truncated = false;
	assert(ReadTail(tailPath, 6, truncated) == "tail");
	assert(truncated);
	assert(Bounded("ab\xE2\x82\xACz", 4) == "ab");

	const std::string firstToken = "ticket_" + std::string(40, 'a');
	const std::string renewedToken = "ticket_" + std::string(40, 'b');
	int registrations = 0;
	int reports = 0;
	std::vector<std::string> submittedTokens;
	const PostFunction post = [&](std::string_view a_url, std::string_view a_body) {
		const auto body = json::parse(a_body);
		if (a_url.ends_with("/v1/installations")) {
			++registrations;
			assert(body.value("clientId", "").starts_with("client_"));
			return Reply(200, {
				{ "installationToken", registrations == 1 ? firstToken : renewedToken },
			});
		}
		++reports;
		submittedTokens.push_back(body.value("installationToken", ""));
		if (reports == 1) return Reply(401, { { "code", "invalid-installation" } });
		return Reply(200, { { "ok", true }, { "reportId", "report-1" } });
	};
	const PayloadFactory payload = [](std::string_view a_clientId, std::string_view a_token) {
		return json{ { "clientId", a_clientId }, { "installationToken", a_token } };
	};

	const auto first = SubmitAuthenticated(endpoint, folder, payload, post);
	assert(first.errorCode.empty());
	assert(first.body.value("reportId", "") == "report-1");
	assert(registrations == 2);
	assert(reports == 2);
	assert((submittedTokens == std::vector<std::string>{ firstToken, renewedToken }));

	const auto second = SubmitAuthenticated(endpoint, folder, payload, post);
	assert(second.errorCode.empty());
	assert(registrations == 2);  // cached renewed ticket
	assert(reports == 3);
	assert(submittedTokens.back() == renewedToken);

	std::filesystem::remove_all(folder);
	std::cout << "reporting core tests passed\n";
}
