#include "Diagnostics/HealthRegistry.h"

#include "Core/Json.h"

namespace OSFUI
{
	bool HealthRegistry::Upsert(IssueSpec a_spec)
	{
		if (a_spec.id.empty() || a_spec.code.empty()) return false;
		const auto found = std::ranges::find(_issues, a_spec.id, &IssueSpec::id);
		if (found != _issues.end() && *found == a_spec) return false;
		// Technical context stays in the native log, never in web state or Settings issue text.
		REX::WARN("WebView health: {} [{}] {} {}", a_spec.id, a_spec.code,
			a_spec.subject, Json::Dump(a_spec.context));
		if (found == _issues.end()) _issues.push_back(std::move(a_spec));
		else *found = std::move(a_spec);
		return true;
	}

	bool HealthRegistry::Resolve(std::string_view a_id)
	{
		return std::erase_if(_issues, [&](const auto& issue) { return issue.id == a_id; }) != 0;
	}

	bool HealthRegistry::IsActive(std::string_view a_id) const
	{
		return std::ranges::find(_issues, a_id, &IssueSpec::id) != _issues.end();
	}
}
