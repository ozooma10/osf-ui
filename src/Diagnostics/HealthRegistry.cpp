#include "Diagnostics/HealthRegistry.h"

#include <cmath>  // not in pch.h

#include "Core/StringUtil.h"
#include "Core/Ids.h"
#include "Core/Json.h"
#include "Bridge/MessageBridge.h"

namespace OSFUI
{
	namespace
	{
		[[nodiscard]] double RoundSeconds(double a_seconds)
		{
			const double clamped = a_seconds > 0.0 ? a_seconds : 0.0;
			return std::round(clamped * 10.0) / 10.0;
		}

		[[nodiscard]] const char* SeverityName(HealthRegistry::Severity a_severity)
		{
			return a_severity == HealthRegistry::Severity::Error ? "error" : "warning";
		}

		[[nodiscard]] int OrderRank(bool a_resolved, HealthRegistry::Severity a_severity)
		{
			const int severityRank = a_severity == HealthRegistry::Severity::Error ? 0 : 1;
			return (a_resolved ? 2 : 0) + severityRank;
		}
	}

	std::string HealthRegistry::RedactPath(std::string_view a_text)
	{
		
		const bool hasSeparator = a_text.find('\\') != std::string_view::npos ||
			a_text.find('/') != std::string_view::npos;
		if (!hasSeparator) {
			return std::string(a_text);
		}
		const auto last = a_text.find_last_of("\\/");
		auto       tail = a_text.substr(last + 1);

		if (tail.empty()) {
			return "<path>";
		}
		return std::string(tail);
	}

	nlohmann::json HealthRegistry::Sanitize(const nlohmann::json& a_context)
	{
		nlohmann::json out = nlohmann::json::object();
		if (!a_context.is_object()) {
			return out;
		}
		std::size_t kept = 0;
		for (const auto& [key, value] : a_context.items()) {
			if (kept >= kMaxContextEntries) {
				break;
			}
			if (value.is_string()) {
				auto text = value.get<std::string>();

				if (key != "consumer" || !Ids::IsValidQualifiedViewId(text)) {
					text = RedactPath(text);
				}
				if (text.size() > kMaxContextValueChars) {
					StringUtil::TruncateUtf8(text, kMaxContextValueChars);
					text += "…";
				}
				out[key] = std::move(text);
			} else if (value.is_number() || value.is_boolean()) {
				out[key] = value;
			} else {
				continue;
			}
			++kept;
		}
		return out;
	}

	HealthRegistry::Issue* HealthRegistry::Find(std::string_view a_id)
	{
		const auto it = std::ranges::find(_issues, a_id, &Issue::id);
		return it == _issues.end() ? nullptr : &*it;
	}

	const HealthRegistry::Issue* HealthRegistry::Find(std::string_view a_id) const
	{
		const auto it = std::ranges::find(_issues, a_id, &Issue::id);
		return it == _issues.end() ? nullptr : &*it;
	}

	bool HealthRegistry::IsActive(std::string_view a_id) const
	{
		const auto* issue = Find(a_id);
		return issue && !issue->resolved;
	}

	bool HealthRegistry::Upsert(const IssueSpec& a_spec, double a_now)
	{
		if (a_spec.id.empty() || a_spec.code.empty()) {
			REX::WARN("HealthRegistry: ignoring an issue with no id/code (code '{}')", a_spec.code);
			return false;
		}
		const double now = RoundSeconds(a_now);
		auto         context = Sanitize(a_spec.context);

		if (auto* existing = Find(a_spec.id)) {
			existing->code = a_spec.code;
			existing->severity = a_spec.severity;
			existing->source = a_spec.source;
			existing->sourceKind = a_spec.sourceKind;
			existing->subject = a_spec.subject;
			existing->context = std::move(context);
			existing->resolved = false;
			existing->resolvedAt = 0.0;
			existing->occurrences += 1;
			existing->lastAt = now;
			++_generation;
			return true;
		}

		_issues.push_back(Issue{
			.id = a_spec.id,
			.code = a_spec.code,
			.severity = a_spec.severity,
			.source = a_spec.source,
			.sourceKind = a_spec.sourceKind,
			.subject = a_spec.subject,
			.context = std::move(context),
			.resolved = false,
			.occurrences = 1,
			.firstAt = now,
			.lastAt = now,
			.resolvedAt = 0.0,
		});
		EnforceCaps();
		++_generation;
		return true;
	}

	bool HealthRegistry::Resolve(std::string_view a_id, double a_now)
	{
		auto* issue = Find(a_id);
		if (!issue || issue->resolved) {
			return false;
		}
		issue->resolved = true;
		issue->resolvedAt = RoundSeconds(a_now);
		EnforceCaps();
		++_generation;
		return true;
	}

	bool HealthRegistry::ResolveMissing(std::string_view a_source,
		const std::unordered_set<std::string>& a_keep, double a_now)
	{
		bool changed = false;
		for (auto& issue : _issues) {
			if (issue.resolved || issue.source != a_source || a_keep.contains(issue.id)) {
				continue;
			}
			issue.resolved = true;
			issue.resolvedAt = RoundSeconds(a_now);
			changed = true;
		}
		if (changed) {
			EnforceCaps();
			++_generation;
		}
		return changed;
	}

	void HealthRegistry::EnforceCaps()
	{
		const auto countOf = [this](bool a_resolved) {
			return static_cast<std::size_t>(
				std::ranges::count(_issues, a_resolved, &Issue::resolved));
		};
		const auto evictOldest = [this](bool a_resolved) {
			const auto it = std::ranges::find(_issues, a_resolved, &Issue::resolved);
			if (it != _issues.end()) {
				REX::WARN("HealthRegistry: evicting {} issue '{}' — history cap reached",
					a_resolved ? "resolved" : "active", it->id);
				_issues.erase(it);

				++_generation;
			}
		};
		while (countOf(true) > kMaxResolvedIssues) {
			evictOldest(true);
		}
		while (countOf(false) > kMaxActiveIssues) {
			evictOldest(false);
		}
	}

	void HealthRegistry::SetSystemInfo(nlohmann::json a_info)
	{
		_system = Sanitize(a_info);
		++_generation;
	}

	nlohmann::json HealthRegistry::Encode(const Issue& a_issue)
	{
		nlohmann::json out{
			{ "id", a_issue.id },
			{ "code", a_issue.code },
			{ "severity", SeverityName(a_issue.severity) },
			{ "status", a_issue.resolved ? "resolved" : "active" },
			{ "source", a_issue.source },
			{ "sourceKind", a_issue.sourceKind == SourceKind::Mod ? "mod" : "platform" },
			{ "subject", a_issue.subject },
			{ "context", a_issue.context },
			{ "occurrences", a_issue.occurrences },
			{ "firstAt", a_issue.firstAt },
			{ "lastAt", a_issue.lastAt },
		};
		if (a_issue.resolved) {
			out["resolvedAt"] = a_issue.resolvedAt;
		}
		return out;
	}

	nlohmann::json HealthRegistry::Snapshot() const
	{
		std::vector<std::size_t> order(_issues.size());
		for (std::size_t i = 0; i < order.size(); ++i) {
			order[i] = i;
		}
		std::ranges::stable_sort(order, [this](std::size_t a_lhs, std::size_t a_rhs) {
			const auto& lhs = _issues[a_lhs];
			const auto& rhs = _issues[a_rhs];
			const int lhsRank = OrderRank(lhs.resolved, lhs.severity);
			const int rhsRank = OrderRank(rhs.resolved, rhs.severity);
			if (lhsRank != rhsRank) {
				return lhsRank < rhsRank;
			}
			const double lhsAt = lhs.resolved ? lhs.resolvedAt : lhs.lastAt;
			const double rhsAt = rhs.resolved ? rhs.resolvedAt : rhs.lastAt;
			if (lhsAt != rhsAt) {
				return lhsAt > rhsAt;
			}
			return a_lhs > a_rhs;
		});

		nlohmann::json issues = nlohmann::json::array();
		for (const auto index : order) {
			issues.push_back(Encode(_issues[index]));
		}
		return nlohmann::json{ { "system", _system }, { "issues", std::move(issues) } };
	}

	void HealthRegistry::Broadcast()
	{
		if (!_bridge) {
			return;
		}

		if (_generation == _sentGeneration) {
			return;
		}
		_sentGeneration = _generation;
		_bridge->PublishStateAll("osfui", "diagnostics", Snapshot());
	}

	void HealthRegistry::RegisterEndpoints(MessageBridge& a_bridge)
	{
		_bridge = &a_bridge;
	}

	void HealthRegistry::OnBridgeDown()
	{
		_bridge = nullptr;
		_sentGeneration = 0;
	}

	void HealthRegistry::OnViewDestroyed(std::string_view)
	{
		// Nothing to prune: delivery is scoped by the bridge's greeted-view set.
	}
}
