#pragma once

#include <unordered_set>  // not in pch.h

#include <nlohmann/json.hpp>

#include "runtime/UiModule.h"

namespace OSFUI
{
	// Session-scoped health registry behind the Mods surface's System Health
	// destination (bridge protocol 1.4).
	//
	// This is NOT a log mirror. Nothing scrapes OSF UI.log; every issue is
	// pushed here explicitly by the subsystem that detected it, and withdrawn
	// explicitly when the condition clears. That is the whole point: the log is
	// a firehose of routine noise, and a player-facing health pane is only
	// useful if everything in it is durable, actionable, and true right now.
	//
	// Identity, not events. An issue is keyed by a stable `id` chosen by the
	// producer (e.g. "view.load-failed:acme/terminal"). Upserting an id that is
	// already active bumps its occurrence count and last-seen time instead of
	// stacking a duplicate card; upserting one that is resolved reactivates the
	// same record, keeping the count. Resolve() moves it to session history —
	// where it stays until the process exits, because "it fixed itself" is
	// exactly the thing a player needs to see after a retry. There is no
	// persistence and no cross-launch history by design.
	//
	// The wire payload carries stable machine `code`s, never player-facing
	// prose: the built-in frontend owns the copy and the offered actions, so
	// they are localizable and cannot be steered by a mod. `context` is bounded
	// technical detail shown only under the card's collapsed disclosure, and is
	// sanitized on the way in — see kMaxContextEntries and Sanitize().
	class DiagnosticsModule final : public IUiModule
	{
	public:
		enum class Severity
		{
			Warning,
			Error,
		};

		// Bounds. A runaway producer must not be able to grow the payload
		// without limit — every push is re-encoded and fanned out to live views.
		static constexpr std::size_t kMaxActiveIssues = 64;
		static constexpr std::size_t kMaxResolvedIssues = 64;
		static constexpr std::size_t kMaxContextEntries = 8;
		static constexpr std::size_t kMaxContextValueChars = 240;

		// What a producer reports. `id` is the dedupe key; everything else is
		// refreshed on every upsert, so a condition that changes severity or
		// detail updates in place.
		struct IssueSpec
		{
			std::string    id;
			std::string    code;      // stable machine code, e.g. "settings.values-parse"
			Severity       severity{ Severity::Warning };
			std::string    source;    // producing subsystem: "settings" | "views" | "host" | "render" | "compat"
			std::string    subject;   // affected mod / view / component id, "" when none
			nlohmann::json context;   // bounded technical detail (object), sanitized on entry
		};

		void OnStart() override {}
		void RegisterCommands(MessageBridge& a_bridge) override;
		void OnBridgeDown() override;
		void OnViewDestroyed(std::string_view a_viewId) override;
		[[nodiscard]] std::string_view Name() const override { return "diagnostics"; }

		// Raise or refresh one condition. Returns true when the snapshot changed
		// (a new issue, a reactivation, or altered fields) — an unchanged repeat
		// still bumps occurrences/lastAt, which counts as a change. `a_now` is
		// the runtime's session clock in seconds (Runtime::_uptime).
		bool Upsert(const IssueSpec& a_spec, double a_now);

		// Withdraw one condition by id. Returns false when it was unknown or
		// already resolved, so callers can resolve unconditionally every tick
		// without generating pushes.
		bool Resolve(std::string_view a_id, double a_now);

		// Resolve every ACTIVE issue of `a_source` whose id is not in
		// `a_keep`. The reconcile primitive for producers that recompute a whole
		// set (settings load errors, targetVersion compatibility): upsert what is
		// wrong now, then sweep away what no longer is. Returns true if anything
		// moved.
		bool ResolveMissing(std::string_view a_source, const std::unordered_set<std::string>& a_keep, double a_now);

		[[nodiscard]] bool IsActive(std::string_view a_id) const;

		// The "System information" block: versions, runtime/host state, renderer
		// and compositor path. Replaced wholesale by the runtime whenever one of
		// its inputs changes; must contain no absolute paths (same rule as
		// context, and enforced the same way).
		void SetSystemInfo(nlohmann::json a_info);

		// { system, issues } — the whole `diagnostics.data` payload. Issues are
		// ordered errors first, then warnings, newest first within a severity;
		// resolved ones sort after every active one.
		[[nodiscard]] nlohmann::json Snapshot() const;

		// Re-send the snapshot to every subscriber, but only when it differs from
		// the last one sent. Callers invoke this unconditionally after any
		// potential change, exactly like Runtime::BroadcastViewsData.
		void Broadcast();

		// Sanitizer, exposed for tests: drops keys past kMaxContextEntries,
		// truncates long strings, and replaces anything that reads as a
		// filesystem path, URL, or command line with its trailing component (or
		// "<path>" when nothing safe remains). Absolute paths identify the
		// player's machine and account, and a shell-shaped string in a payload
		// the frontend renders is an invitation to build an "open this" button
		// around it — neither belongs on the wire (docs/security-model.md).
		[[nodiscard]] static nlohmann::json Sanitize(const nlohmann::json& a_context);

		// Redact one string the same way Sanitize() redacts context values.
		[[nodiscard]] static std::string RedactPath(std::string_view a_text);

	private:
		struct Issue
		{
			std::string    id;
			std::string    code;
			Severity       severity{ Severity::Warning };
			std::string    source;
			std::string    subject;
			nlohmann::json context;
			bool           resolved{ false };
			std::uint32_t  occurrences{ 0 };
			double         firstAt{ 0.0 };
			double         lastAt{ 0.0 };
			double         resolvedAt{ 0.0 };
		};

		[[nodiscard]] Issue*       Find(std::string_view a_id);
		[[nodiscard]] const Issue* Find(std::string_view a_id) const;

		// Drop the oldest resolved (then oldest active) records once the caps are
		// hit, so a pathological producer trims history rather than the payload
		// growing unboundedly.
		void EnforceCaps();

		[[nodiscard]] static nlohmann::json Encode(const Issue& a_issue);

		// Insertion order is the tiebreaker for "newest first" within a severity;
		// a vector keeps that without a second timestamp comparison.
		std::vector<Issue>              _issues;
		nlohmann::json                  _system = nlohmann::json::object();
		MessageBridge*                  _bridge{ nullptr };
		std::unordered_set<std::string> _subscribers;
		std::string                     _lastSent;  // dedupe: last payload dump
	};
}
