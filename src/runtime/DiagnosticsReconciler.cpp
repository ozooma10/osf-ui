#include "runtime/DiagnosticsReconciler.h"

#include <algorithm>
#include <tuple>
#include <unordered_set>

namespace OSFUI
{
	void DiagnosticsReconciler::SyncSettings(DiagnosticsModule& a_diagnostics,
		std::span<const SettingsLoadIssue> a_errors, double a_now)
	{
		std::unordered_set<std::string> live;
		for (const auto& error : a_errors) {
			const auto subject = error.mod.empty() ? error.file : error.mod;
			auto id = "settings." + error.kind + ':' + subject;
			live.insert(id);
			a_diagnostics.Upsert(DiagnosticsModule::IssueSpec{
				.id = std::move(id),
				.code = "settings." + error.kind,
				.severity = error.kind == "values-parse" ?
					DiagnosticsModule::Severity::Warning : DiagnosticsModule::Severity::Error,
				.source = "settings",
				.subject = subject,
				.context = nlohmann::json{
					{ "file", error.file },
					{ "message", error.message },
				},
			}, a_now);
		}
		a_diagnostics.ResolveMissing("settings", live, a_now);
	}

	void DiagnosticsReconciler::SyncCompatibility(DiagnosticsModule& a_diagnostics,
		std::span<const CompatibilityTarget> a_targets,
		std::string_view a_installedVersion, double a_now)
	{
		std::vector targets(a_targets.begin(), a_targets.end());
		std::ranges::sort(targets, {}, [](const CompatibilityTarget& a_item) {
			return std::tie(a_item.kind, a_item.id, a_item.targetVersion);
		});
		targets.erase(std::unique(targets.begin(), targets.end(),
			[](const auto& a_lhs, const auto& a_rhs) {
				return std::tie(a_lhs.kind, a_lhs.id, a_lhs.targetVersion) ==
					std::tie(a_rhs.kind, a_rhs.id, a_rhs.targetVersion);
			}), targets.end());

		std::string signature;
		for (const auto& item : targets) {
			signature += item.kind + ':' + item.id + '@' + item.targetVersion + ';';
		}
		if (signature == _compatSignature) return;
		_compatSignature = std::move(signature);

		std::unordered_set<std::string> live;
		for (const auto& item : targets) {
			auto id = "compat.needs-newer-osfui:" + item.kind + ':' + item.id;
			live.insert(id);
			a_diagnostics.Upsert(DiagnosticsModule::IssueSpec{
				.id = std::move(id),
				.code = "compat.needs-newer-osfui",
				.severity = DiagnosticsModule::Severity::Warning,
				.source = "compat",
				.subject = item.id,
				.context = nlohmann::json{
					{ "kind", item.kind },
					{ "targetVersion", item.targetVersion },
					{ "installedVersion", a_installedVersion },
				},
			}, a_now);
		}
		a_diagnostics.ResolveMissing("compat", live, a_now);
	}

	void DiagnosticsReconciler::ReportViewLoad(DiagnosticsModule& a_diagnostics,
		std::string_view a_viewId, bool a_failed, std::string_view a_description,
		int a_errorCode, std::uint32_t a_attemptsLeft, double a_now)
	{
		const std::string id(a_viewId);
		const auto retrying = "view.load-retrying:" + id;
		const auto failed = "view.load-failed:" + id;
		if (!a_failed) {
			a_diagnostics.Resolve(retrying, a_now);
			a_diagnostics.Resolve(failed, a_now);
			return;
		}
		nlohmann::json context{
			{ "errorCode", a_errorCode },
			{ "description", a_description },
			{ "attemptsLeft", a_attemptsLeft },
		};
		if (a_attemptsLeft > 0) {
			a_diagnostics.Upsert(DiagnosticsModule::IssueSpec{
				.id = retrying,
				.code = "view.load-retrying",
				.severity = DiagnosticsModule::Severity::Warning,
				.source = "views",
				.subject = id,
				.context = std::move(context),
			}, a_now);
			return;
		}
		a_diagnostics.Resolve(retrying, a_now);
		a_diagnostics.Upsert(DiagnosticsModule::IssueSpec{
			.id = failed,
			.code = "view.load-failed",
			.severity = DiagnosticsModule::Severity::Error,
			.source = "views",
			.subject = id,
			.context = std::move(context),
		}, a_now);
	}
}
