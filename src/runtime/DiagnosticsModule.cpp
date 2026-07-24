#include "runtime/DiagnosticsModule.h"

#include <cmath>  // not in pch.h

#include "runtime/MessageBridge.h"

namespace OSFUI
{
	namespace
	{
		// Session-relative seconds, rounded to a tenth. The frontend renders
		// these as "3s ago" style relative times; more precision would only make
		// the dedupe dump churn.
		[[nodiscard]] double RoundSeconds(double a_seconds)
		{
			const double clamped = a_seconds > 0.0 ? a_seconds : 0.0;
			return std::round(clamped * 10.0) / 10.0;
		}

		[[nodiscard]] const char* SeverityName(DiagnosticsModule::Severity a_severity)
		{
			return a_severity == DiagnosticsModule::Severity::Error ? "error" : "warning";
		}

		// Rank for the wire ordering: errors before warnings, active before
		// resolved. Lower sorts first.
		[[nodiscard]] int OrderRank(bool a_resolved, DiagnosticsModule::Severity a_severity)
		{
			const int severityRank = a_severity == DiagnosticsModule::Severity::Error ? 0 : 1;
			return (a_resolved ? 2 : 0) + severityRank;
		}
	}

	std::string DiagnosticsModule::RedactPath(std::string_view a_text)
	{
		// Path-shaped: a drive letter ("C:\"), a UNC/POSIX root, a URL scheme, or
		// simply any embedded separator. Producers are supposed to pass bare
		// filenames and ids, so this is a backstop against a message that
		// interpolated a full path — the file's own name is the part a player
		// (or a bug report) can act on, and the directory chain is their machine.
		const bool hasSeparator = a_text.find('\\') != std::string_view::npos ||
			a_text.find('/') != std::string_view::npos;
		if (!hasSeparator) {
			return std::string(a_text);
		}
		const auto last = a_text.find_last_of("\\/");
		auto       tail = a_text.substr(last + 1);
		// A trailing separator (a directory) leaves nothing behind; so does a
		// bare root. Say so rather than emitting an empty string that reads as a
		// missing field.
		if (tail.empty()) {
			return "<path>";
		}
		return std::string(tail);
	}

	nlohmann::json DiagnosticsModule::Sanitize(const nlohmann::json& a_context)
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
				auto text = RedactPath(value.get<std::string>());
				if (text.size() > kMaxContextValueChars) {
					text.resize(kMaxContextValueChars);
					text += "…";
				}
				out[key] = std::move(text);
			} else if (value.is_number() || value.is_boolean()) {
				out[key] = value;
			} else {
				// Arrays/objects/null: no v1 producer needs them, and allowing
				// them would reopen the size question one nesting level down.
				continue;
			}
			++kept;
		}
		return out;
	}

	DiagnosticsModule::Issue* DiagnosticsModule::Find(std::string_view a_id)
	{
		const auto it = std::ranges::find(_issues, a_id, &Issue::id);
		return it == _issues.end() ? nullptr : &*it;
	}

	const DiagnosticsModule::Issue* DiagnosticsModule::Find(std::string_view a_id) const
	{
		const auto it = std::ranges::find(_issues, a_id, &Issue::id);
		return it == _issues.end() ? nullptr : &*it;
	}

	bool DiagnosticsModule::IsActive(std::string_view a_id) const
	{
		const auto* issue = Find(a_id);
		return issue && !issue->resolved;
	}

	bool DiagnosticsModule::Upsert(const IssueSpec& a_spec, double a_now)
	{
		if (a_spec.id.empty() || a_spec.code.empty()) {
			REX::WARN("Diagnostics: ignoring an issue with no id/code (code '{}')", a_spec.code);
			return false;
		}
		const double now = RoundSeconds(a_now);
		auto         context = Sanitize(a_spec.context);

		if (auto* existing = Find(a_spec.id)) {
			// Same identity: this is a recurrence, not a new condition. The
			// occurrence count is what tells "it happened once at startup" apart
			// from "it is happening every few seconds".
			existing->code = a_spec.code;
			existing->severity = a_spec.severity;
			existing->source = a_spec.source;
			existing->subject = a_spec.subject;
			existing->context = std::move(context);
			existing->resolved = false;
			existing->resolvedAt = 0.0;
			existing->occurrences += 1;
			existing->lastAt = now;
			return true;
		}

		_issues.push_back(Issue{
			.id = a_spec.id,
			.code = a_spec.code,
			.severity = a_spec.severity,
			.source = a_spec.source,
			.subject = a_spec.subject,
			.context = std::move(context),
			.resolved = false,
			.occurrences = 1,
			.firstAt = now,
			.lastAt = now,
			.resolvedAt = 0.0,
		});
		EnforceCaps();
		return true;
	}

	bool DiagnosticsModule::Resolve(std::string_view a_id, double a_now)
	{
		auto* issue = Find(a_id);
		if (!issue || issue->resolved) {
			return false;
		}
		issue->resolved = true;
		issue->resolvedAt = RoundSeconds(a_now);
		EnforceCaps();
		return true;
	}

	bool DiagnosticsModule::ResolveMissing(std::string_view a_source,
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
		}
		return changed;
	}

	void DiagnosticsModule::EnforceCaps()
	{
		const auto countOf = [this](bool a_resolved) {
			return static_cast<std::size_t>(
				std::ranges::count(_issues, a_resolved, &Issue::resolved));
		};
		// Oldest-first eviction within each bucket: _issues is in insertion
		// order, so the first match is the oldest record of that kind.
		const auto evictOldest = [this](bool a_resolved) {
			const auto it = std::ranges::find(_issues, a_resolved, &Issue::resolved);
			if (it != _issues.end()) {
				REX::WARN("Diagnostics: evicting {} issue '{}' — history cap reached",
					a_resolved ? "resolved" : "active", it->id);
				_issues.erase(it);
			}
		};
		while (countOf(true) > kMaxResolvedIssues) {
			evictOldest(true);
		}
		while (countOf(false) > kMaxActiveIssues) {
			evictOldest(false);
		}
	}

	void DiagnosticsModule::SetSystemInfo(nlohmann::json a_info)
	{
		_system = Sanitize(a_info);
	}

	nlohmann::json DiagnosticsModule::Encode(const Issue& a_issue)
	{
		nlohmann::json out{
			{ "id", a_issue.id },
			{ "code", a_issue.code },
			{ "severity", SeverityName(a_issue.severity) },
			{ "status", a_issue.resolved ? "resolved" : "active" },
			{ "source", a_issue.source },
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

	nlohmann::json DiagnosticsModule::Snapshot() const
	{
		// Index vector rather than copying records: ordering only needs the rank
		// and the insertion position.
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
			// Newest first: the moment it last happened, then insertion order as
			// the stable tiebreaker for issues raised in the same tick.
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

	void DiagnosticsModule::Broadcast()
	{
		if (!_bridge || _subscribers.empty()) {
			return;
		}
		auto dumped = Snapshot().dump();
		if (dumped == _lastSent) {
			return;
		}
		_lastSent = std::move(dumped);
		_bridge->SendJsonToWeb(_subscribers, "diagnostics.data", _lastSent);
	}

	void DiagnosticsModule::RegisterCommands(MessageBridge& a_bridge)
	{
		_bridge = &a_bridge;
		// Subscribe-on-read, the `views.get` / `settings.get` pattern: the reply
		// is the current snapshot and the caller keeps receiving `diagnostics.data`
		// for every later change.
		a_bridge.RegisterCommand("diagnostics.get", [this](const nlohmann::json&, MessageBridge& a_b) {
			const auto source = std::string(a_b.CurrentSource());
			if (!source.empty()) {
				_subscribers.insert(source);
			}
			const auto payload = Snapshot();
			// Seed the dedupe with what this caller just received, so the very
			// next Broadcast() doesn't re-send an identical document.
			_lastSent = payload.dump();
			a_b.SendToWeb("diagnostics.data", payload);
		});
	}

	void DiagnosticsModule::OnBridgeDown()
	{
		_bridge = nullptr;
		_subscribers.clear();
		_lastSent.clear();
	}

	void DiagnosticsModule::OnViewDestroyed(std::string_view a_viewId)
	{
		_subscribers.erase(std::string(a_viewId));
	}
}
