#pragma once

#include <span>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace OSFUI
{
	// Only OSF UI's current WebView conditions. OSF Settings owns the shared registry and UI.
	class HealthRegistry final
	{
	public:
		enum class Severity { Warning, Error };
		struct IssueSpec
		{
			std::string id;
			std::string code;
			Severity severity{ Severity::Warning };
			std::string subject;
			nlohmann::json context;
			bool operator==(const IssueSpec&) const = default;
		};

		bool Upsert(IssueSpec a_spec);
		bool Resolve(std::string_view a_id);
		[[nodiscard]] bool IsActive(std::string_view a_id) const;
		[[nodiscard]] std::span<const IssueSpec> ActiveIssues() const { return _issues; }

	private:
		std::vector<IssueSpec> _issues;
	};
}
