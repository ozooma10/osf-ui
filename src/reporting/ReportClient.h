#pragma once

#include <nlohmann/json.hpp>

namespace OSFUI::Reporting
{
	struct SubmissionResult
	{
		bool        ok{ false };
		std::string code;
		std::string message;
		std::string reportId;
		std::string issueUrl;
		std::uint64_t issueNumber{ 0 };
	};

	// Collect bounded log tails, redact local account/install roots, and post a
	// report the player explicitly consented to. Synchronous by design; callers
	// must run it away from the game thread.
	[[nodiscard]] SubmissionResult Submit(
		std::string_view a_endpoint,
		const nlohmann::json& a_diagnostics,
		std::string_view a_title,
		std::string_view a_description,
		std::string_view a_reproduction);
}
