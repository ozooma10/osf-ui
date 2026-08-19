#pragma once

#include <span>
#include <unordered_set>  // not in pch.h

#include <nlohmann/json.hpp>

namespace OSFUI
{
	class MessageBridge;

	class HealthRegistry final
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
			std::string    scope;     // internal reconciliation owner; never serialized
		};

		void AttachBridge(MessageBridge& a_bridge);
		void DetachBridge();

		// Record another observation of one condition. Repeated observations increment occurrences and refresh lastAt, even when every descriptive field is unchanged.
		bool Upsert(const IssueSpec& a_spec, double a_now);

		// Replace the complete current set owned by a_scope. Unchanged active issues remain untouched; newly present/reactivated/changed issues are updated, and missing ones are resolved
		bool ReplaceScope(std::string_view a_scope, std::span<const IssueSpec> a_specs, double a_now);

		// Withdraw one condition by id. Returns false when it was unknown or already resolved, so callers can resolve unconditionally every tick without generating pushes.
		bool Resolve(std::string_view a_id, double a_now);

		// Sweep-only form used by the native ABI's ordered Report/ClearExcept queue.
		bool ResolveMissingInScope(std::string_view a_scope, const std::unordered_set<std::string>& a_keep, double a_now);

		bool IsActive(std::string_view a_id) const;

		bool SetSystemInfo(nlohmann::json a_info);

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
			std::string    scope;
			bool           resolved{ false };
			std::uint32_t  occurrences{ 0 };
			double         firstAt{ 0.0 };
			double         lastAt{ 0.0 };
			double         resolvedAt{ 0.0 };
		};

		[[nodiscard]] Issue*       Find(std::string_view a_id);
		[[nodiscard]] const Issue* Find(std::string_view a_id) const;
		bool Apply(const IssueSpec& a_spec, double a_now, bool a_recordOccurrence, std::string_view a_scope);

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
