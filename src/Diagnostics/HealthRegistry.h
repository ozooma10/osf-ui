#pragma once

#include <unordered_set>  // not in pch.h

#include <nlohmann/json.hpp>

#include "Runtime/UiModule.h"

namespace OSFUI
{
	// Session-scoped health registry behind Mod Settings' System Health destination (introduced in web bridge protocol 1.4).
	class HealthRegistry final : public IUiModule
	{
	public:
		enum class Severity
		{
			Warning,
			Error,
		};
		enum class SourceKind
		{
			Platform,
			Mod,
		};

		static constexpr std::size_t kMaxActiveIssues = 64;
		static constexpr std::size_t kMaxResolvedIssues = 64;
		static constexpr std::size_t kMaxContextEntries = 8;
		static constexpr std::size_t kMaxContextValueChars = 240;

		struct IssueSpec
		{
			std::string    id;
			std::string    code;      // stable machine code, e.g. "settings.values-parse"
			Severity       severity{ Severity::Warning };
			std::string    source;    // subsystem; "host" is the browser-host/web-renderer compatibility value
			SourceKind     sourceKind{ SourceKind::Platform };
			std::string    subject;   // affected mod / view / component id, "" when none
			nlohmann::json context;   // bounded technical detail (object), sanitized on entry
		};

		void RegisterEndpoints(MessageBridge& a_bridge) override;
		void OnBridgeDown() override;
		void OnViewDestroyed(std::string_view a_viewId) override;
		[[nodiscard]] std::string_view Name() const override { return "health"; }

		// Raise or refresh one condition. Returns true when the snapshot changed (a new issue, a reactivation, or altered fields
		bool Upsert(const IssueSpec& a_spec, double a_now);

		// Withdraw one condition by id. Returns false when it was unknown or already resolved, so callers can resolve unconditionally every tick without generating pushes.
		bool Resolve(std::string_view a_id, double a_now);

		// Resolve every ACTIVE issue of `a_source` whose id is not in `a_keep`. The reconcile primitive for producers that recompute a whole set (settings load errors, targetVersion compatibility): 
		// upsert what is wrong now, then sweep away what no longer is. Returns true if anything moved.
		bool ResolveMissing(std::string_view a_source, const std::unordered_set<std::string>& a_keep, double a_now);

		[[nodiscard]] bool IsActive(std::string_view a_id) const;

		void SetSystemInfo(nlohmann::json a_info);

		// { system, issues } - `osfui/diagnostics` state value. Issues are ordered errors first, then warnings, newest first within a severity; resolved ones sort after every active one.
		[[nodiscard]] nlohmann::json Snapshot() const;

		void Broadcast();

		[[nodiscard]] static nlohmann::json Sanitize(const nlohmann::json& a_context);
		[[nodiscard]] static std::string RedactPath(std::string_view a_text);

	private:
		struct Issue
		{
			std::string    id;
			std::string    code;
			Severity       severity{ Severity::Warning };
			std::string    source;
			SourceKind     sourceKind{ SourceKind::Platform };
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

		void EnforceCaps();

		[[nodiscard]] static nlohmann::json Encode(const Issue& a_issue);

		// Insertion order is the tiebreaker for "newest first" within a severity; a vector keeps that without a second timestamp comparison.
		std::vector<Issue>              _issues;
		nlohmann::json                  _system = nlohmann::json::object();
		MessageBridge*                  _bridge{ nullptr };

		std::uint64_t                   _generation{ 1 };
		std::uint64_t                   _sentGeneration{ 0 };
	};
}
