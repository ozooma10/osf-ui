#include "Diagnostics/HealthReconciler.h"

#include "Core/Log.h"

#include <cassert>
#include <iostream>

namespace OSFUI::Log
{
	void WarnOnce(std::once_flag& a_flag, std::string_view a_message)
	{
		std::call_once(a_flag, [&] { REX::test::Log("WARN", std::string(a_message)); });
	}
	bool DebugEnabled() { return true; }
	void SetDebugLogging(bool) {}
}

namespace
{
	nlohmann::json IssueById(const OSFUI::HealthRegistry& a_healthRegistry,
		std::string_view a_id)
	{
		const auto snapshot = a_healthRegistry.Snapshot();
		for (const auto& issue : snapshot.at("issues")) {
			if (issue.value("id", "") == a_id) return issue;
		}
		return {};
	}
}

int main()
{
	using namespace OSFUI;
	HealthRegistry healthRegistry;
	HealthReconciler reconciler;

	const std::array settingsErrors{
		SettingsLoadIssue{ "values-parse", "values.json", "acme", "bad value" },
		SettingsLoadIssue{ "schema-parse", "schema.json", "beta", "bad schema" },
		SettingsLoadIssue{ "hotkey-target", "acme.mod.json", "acme.mod.startScene", "missing target",
			nlohmann::json{ { "script", "Acme_Hotkeys" }, { "function", "OnHotkey" } } },
	};
	reconciler.SyncSettings(healthRegistry, settingsErrors, 1.0);
	assert(healthRegistry.IsActive("settings.values-parse:acme"));
	assert(healthRegistry.IsActive("settings.schema-parse:beta"));
	assert(IssueById(healthRegistry, "settings.values-parse:acme").value("severity", "") == "warning");
	assert(IssueById(healthRegistry, "settings.schema-parse:beta").value("severity", "") == "error");
	const auto hotkeyId = "settings.hotkey-target:acme.mod.startScene";
	assert(healthRegistry.IsActive(hotkeyId));
	assert(IssueById(healthRegistry, hotkeyId).at("context").value("script", "") == "Acme_Hotkeys");
	const auto hotkeyOccurrences = IssueById(healthRegistry, hotkeyId).value("occurrences", 0u);
	reconciler.SyncSettings(healthRegistry, settingsErrors, 1.5);
	assert(IssueById(healthRegistry, hotkeyId).value("occurrences", 0u) == hotkeyOccurrences);
	reconciler.SyncSettings(healthRegistry, {}, 2.0);
	assert(!healthRegistry.IsActive("settings.values-parse:acme"));
	assert(!healthRegistry.IsActive("settings.schema-parse:beta"));
	assert(!healthRegistry.IsActive(hotkeyId));

	const std::array targets{
		CompatibilityTarget{ "beta/mod", "mod", "2.0.0", "compat.needs-newer-osfui",
			HealthRegistry::Severity::Error, "targetVersion" },
		CompatibilityTarget{ "acme/view", "view", "2.0.0", "compat.needs-newer-osfui",
			HealthRegistry::Severity::Error, "targetVersion" },
	};
	reconciler.SyncCompatibility(healthRegistry, targets, "1.5.0", 3.0);
	const auto compatId = "compat.needs-newer-osfui:view:acme/view";
	assert(healthRegistry.IsActive(compatId));
	assert(IssueById(healthRegistry, compatId).at("context").value("installedVersion", "") == "1.5.0");
	const std::array unsupportedConsumers{
		CompatibilityTarget{ "FuturePlugin.dll", "plugin", "3.0", "compat.unsupported-api",
			HealthRegistry::Severity::Error, "abi" },
	};
	reconciler.SyncCompatibility(healthRegistry, unsupportedConsumers, "2.0.0", 3.5);
	const auto unsupported = IssueById(healthRegistry, "compat.unsupported-api:plugin:FuturePlugin.dll");
	assert(unsupported.value("severity", "") == "error");
	assert(unsupported.at("context").value("abi", "") == "3.0");
	reconciler.SyncCompatibility(healthRegistry, targets, "1.5.0", 3.75);
	const auto occurrences = IssueById(healthRegistry, compatId).value("occurrences", 0u);
	const std::array reordered{ targets[1], targets[0] };
	reconciler.SyncCompatibility(healthRegistry, reordered, "1.5.0", 4.0);
	assert(IssueById(healthRegistry, compatId).value("occurrences", 0u) == occurrences);
	reconciler.SyncCompatibility(healthRegistry,
		std::span<const CompatibilityTarget>{ targets.data(), 1 }, "1.5.0", 5.0);
	assert(!healthRegistry.IsActive(compatId));

	reconciler.ReportViewLoad(healthRegistry, "acme/view", true, "timeout", -1, 2, 6.0);
	assert(healthRegistry.IsActive("view.load-retrying:acme/view"));
	reconciler.ReportViewLoad(healthRegistry, "acme/view", true, "timeout", -1, 0, 7.0);
	assert(!healthRegistry.IsActive("view.load-retrying:acme/view"));
	assert(healthRegistry.IsActive("view.load-failed:acme/view"));
	reconciler.ReportViewLoad(healthRegistry, "acme/view", false, "", 0, 0, 8.0);
	assert(!healthRegistry.IsActive("view.load-failed:acme/view"));

	reconciler.SyncControlMap(healthRegistry, false, "1.16.244.0", "layout mismatch", 9.0);
	const auto controlMapId = "input.control-map-unavailable";
	assert(healthRegistry.IsActive(controlMapId));
	const auto controlMapOccurrences = IssueById(healthRegistry, controlMapId).value("occurrences", 0u);
	reconciler.SyncControlMap(healthRegistry, false, "1.16.244.0", "layout mismatch", 10.0);
	assert(IssueById(healthRegistry, controlMapId).value("occurrences", 0u) == controlMapOccurrences);
	reconciler.SyncControlMap(healthRegistry, true, "1.16.244.0", "", 11.0);
	assert(!healthRegistry.IsActive(controlMapId));

	reconciler.ReportRendererHealth(healthRegistry, "host.ring-truncated", true,
		"mixed helper", true, 12.0);
	assert(healthRegistry.IsActive("host.ring-truncated"));
	assert(IssueById(healthRegistry, "host.ring-truncated").at("context").value("renderer", "") == "webview2");
	reconciler.ReportRendererHealth(healthRegistry, "host.ring-truncated", false,
		"", true, 13.0);
	assert(!healthRegistry.IsActive("host.ring-truncated"));

	reconciler.ReportProtocolMisuse(healthRegistry, "acme/view", "unknown-endpoint", 10, 14.0);
	assert(healthRegistry.IsActive("view.protocol-misuse:acme/view"));

	std::cout << "runtime health tests passed\n";
}
