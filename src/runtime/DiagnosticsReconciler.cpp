#include "runtime/DiagnosticsReconciler.h"

#include <algorithm>
#include <tuple>
#include <unordered_set>

namespace OSFUI
{
	void DiagnosticsReconciler::SyncSettings(DiagnosticsModule& a_diagnostics,
		std::span<const SettingsLoadIssue> a_errors, double a_now)
	{
		std::vector<std::string> signatureParts;
		signatureParts.reserve(a_errors.size());
		for (const auto& error : a_errors) {
			signatureParts.push_back(error.kind + "\n" + error.file + "\n" + error.subject + "\n" +
				error.message + "\n" + error.context.dump());
		}
		std::ranges::sort(signatureParts);
		std::string signature;
		for (const auto& part : signatureParts) {
			signature += part;
			signature.push_back('\0');
		}
		if (signature == _settingsSignature) {
			return;
		}
		_settingsSignature = std::move(signature);

		std::unordered_set<std::string> live;
		for (const auto& error : a_errors) {
			const auto subject = error.subject.empty() ? error.file : error.subject;
			auto id = "settings." + error.kind + ':' + subject;
			live.insert(id);
			auto context = error.context.is_object() ? error.context : nlohmann::json::object();
			if (!error.file.empty()) {
				context["file"] = error.file;
			}
			context["message"] = error.message;
			a_diagnostics.Upsert(DiagnosticsModule::IssueSpec{
				.id = std::move(id),
				.code = "settings." + error.kind,
				.severity = error.kind == "values-parse" ?
					DiagnosticsModule::Severity::Warning : DiagnosticsModule::Severity::Error,
				.source = "settings",
				.subject = subject,
				.context = std::move(context),
			}, a_now);
		}
		a_diagnostics.ResolveMissing("settings", live, a_now);
	}

	void DiagnosticsReconciler::SyncCompatibility(DiagnosticsModule& a_diagnostics,
		std::span<const CompatibilityTarget> a_targets,
		std::string_view a_installedVersion, double a_now)
	{
		std::vector signatureTargets(a_targets.begin(), a_targets.end());
		std::ranges::sort(signatureTargets, {}, [](const CompatibilityTarget& a_item) {
			return std::tie(a_item.code, a_item.kind, a_item.id, a_item.targetVersion);
		});
		signatureTargets.erase(std::unique(signatureTargets.begin(), signatureTargets.end(),
			[](const auto& a_lhs, const auto& a_rhs) {
				return std::tie(a_lhs.code, a_lhs.kind, a_lhs.id, a_lhs.targetVersion) ==
					std::tie(a_rhs.code, a_rhs.kind, a_rhs.id, a_rhs.targetVersion);
			}), signatureTargets.end());

		std::string signature;
		for (const auto& item : signatureTargets) {
			signature += item.code + '|' + item.kind + ':' + item.id + '@' + item.targetVersion + ';';
		}
		if (signature == _compatSignature) return;
		_compatSignature = std::move(signature);

		std::unordered_set<std::string> live;
		for (const auto& item : a_targets) {
			auto id = item.code + ':' + item.kind + ':' + item.id;
			live.insert(id);
			a_diagnostics.Upsert(DiagnosticsModule::IssueSpec{
				.id = std::move(id),
				.code = item.code,
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
