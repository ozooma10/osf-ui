#include "Diagnostics/HealthReconciler.h"

#include <array>

namespace OSFUI
{
	void HealthReconciler::SyncSettings(HealthRegistry& a_healthRegistry, std::span<const SettingsLoadIssue> a_errors, double a_now)
	{
		std::vector<HealthRegistry::IssueSpec> issues;
		issues.reserve(a_errors.size());
		for (const auto& error : a_errors) {
			const auto subject = error.subject.empty() ? error.file : error.subject;
			auto id = "settings." + error.kind + ':' + subject;
			auto context = error.context.is_object() ? error.context : nlohmann::json::object();
			if (!error.file.empty()) {
				context["file"] = error.file;
			}
			context["message"] = error.message;
			issues.push_back(HealthRegistry::IssueSpec{
				.id = std::move(id),
				.code = "settings." + error.kind,
				.severity = error.kind == "values-parse" ? HealthRegistry::Severity::Warning : HealthRegistry::Severity::Error,
				.source = "settings",
				.subject = subject,
				.context = std::move(context),
			});
		}
		a_healthRegistry.ReplaceScope("settings", issues, a_now);
	}

	void HealthReconciler::SyncCompatibility(HealthRegistry& a_healthRegistry, std::span<const CompatibilityTarget> a_targets, std::string_view a_installedVersion, double a_now)
	{
		std::vector<HealthRegistry::IssueSpec> issues;
		issues.reserve(a_targets.size());
		for (const auto& item : a_targets) {
			auto id = item.code + ':' + item.kind + ':' + item.id;
			nlohmann::json context{
				{ "kind", item.kind },
				{ "consumer", item.id },
				{ "installedVersion", a_installedVersion },
			};
			context[item.declaration] = item.targetVersion;
			issues.push_back(HealthRegistry::IssueSpec{
				.id = std::move(id),
				.code = item.code,
				.severity = item.severity,
				.source = "compat",
				.subject = item.id,
				.context = std::move(context),
			});
		}
		a_healthRegistry.ReplaceScope("compat", issues, a_now);
	}

	void HealthReconciler::ReportViewLoad(HealthRegistry& a_healthRegistry, std::string_view a_viewId, bool a_failed, std::string_view a_description, int a_errorCode, std::uint32_t a_attemptsLeft, double a_now)
	{
		const std::string id(a_viewId);
		const auto retrying = "view.load-retrying:" + id;
		const auto failed = "view.load-failed:" + id;
		if (!a_failed) {
			a_healthRegistry.Resolve(retrying, a_now);
			a_healthRegistry.Resolve(failed, a_now);
			return;
		}
		nlohmann::json context{
			{ "errorCode", a_errorCode },
			{ "description", a_description },
			{ "attemptsLeft", a_attemptsLeft },
		};
		if (a_attemptsLeft > 0) {
			a_healthRegistry.Upsert(HealthRegistry::IssueSpec{
				.id = retrying,
				.code = "view.load-retrying",
				.severity = HealthRegistry::Severity::Warning,
				.source = "views",
				.subject = id,
				.context = std::move(context),
			}, a_now);
			return;
		}
		a_healthRegistry.Resolve(retrying, a_now);
		a_healthRegistry.Upsert(HealthRegistry::IssueSpec{
			.id = failed,
			.code = "view.load-failed",
			.severity = HealthRegistry::Severity::Error,
			.source = "views",
			.subject = id,
			.context = std::move(context),
		}, a_now);
	}

	void HealthReconciler::SyncControlMap(HealthRegistry& a_healthRegistry, bool a_available, std::string_view a_gameVersion, std::string_view a_reason, double a_now)
	{
		if (a_available) {
			a_healthRegistry.ReplaceScope("input.control-map", std::span<const HealthRegistry::IssueSpec>{}, a_now);
			return;
		}
		const std::array issues{
			HealthRegistry::IssueSpec{
				.id = "input.control-map-unavailable",
				.code = "input.control-map-unavailable",
				.severity = HealthRegistry::Severity::Warning,
				.source = "input",
				.subject = "Starfield ControlMap",
				.context = {
					{ "gameVersion", std::string(a_gameVersion) },
					{ "reason", std::string(a_reason) },
				},
			},
		};
		a_healthRegistry.ReplaceScope("input.control-map", issues, a_now);
	}

	void HealthReconciler::ReportRendererHealth(HealthRegistry& a_healthRegistry, std::string_view a_code, bool a_active, std::string_view a_detail, bool a_rendererAvailable, double a_now)
	{
		if (a_code.empty()) return;
		if (!a_active) {
			a_healthRegistry.Resolve(a_code, a_now);
			return;
		}
		nlohmann::json context = nlohmann::json::object();
		if (!a_detail.empty()) context["detail"] = std::string(a_detail);
		context["renderer"] = a_rendererAvailable ? "webview2" : "none";
		a_healthRegistry.Upsert(HealthRegistry::IssueSpec{
			.id = std::string(a_code),
			.code = std::string(a_code),
			.severity = HealthRegistry::Severity::Warning,
			.source = "host",
			.subject = a_rendererAvailable ? "webview2" : std::string{},
			.context = std::move(context),
		}, a_now);
	}

	void HealthReconciler::ReportProtocolMisuse(HealthRegistry& a_healthRegistry, std::string_view a_viewId, std::string_view a_code, std::uint32_t a_count, double a_now)
	{
		if (a_viewId.empty()) return;
		a_healthRegistry.Upsert(HealthRegistry::IssueSpec{
			.id = std::format("view.protocol-misuse:{}", a_viewId),
			.code = "view.protocol-misuse",
			.severity = HealthRegistry::Severity::Warning,
			.source = "views",
			.subject = std::string(a_viewId),
			.context = nlohmann::json{
				{ "code", std::string(a_code) },
				{ "count", a_count },
			},
		}, a_now);
	}
}
