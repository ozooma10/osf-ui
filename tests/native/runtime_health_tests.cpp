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
	bool DevMode() { return true; }
	void SetDevMode(bool) {}
}

namespace
{
	nlohmann::json IssueById(const OSFUI::HealthRegistry& a_healthRegistry,
		std::string_view a_id)
	{
		// Bind the snapshot: at("issues") returns a reference into the returned
		// temporary, and range-for lifetime extension (P2718) is not implemented
		// by every supported C++23 toolchain.
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
			HealthRegistry::Severity::Error, "", "targetVersion" },
		CompatibilityTarget{ "acme/view", "view", "2.0.0", "compat.needs-newer-osfui",
			HealthRegistry::Severity::Error, "", "targetVersion" },
		CompatibilityTarget{ "legacy.mod/panel", "view", "1.9.0", "compat.pre-2-view",
			HealthRegistry::Severity::Warning, "2.1.0" },
	};
	reconciler.SyncCompatibility(healthRegistry, targets, "1.5.0", 3.0);
	const auto compatId = "compat.needs-newer-osfui:view:acme/view";
	assert(healthRegistry.IsActive(compatId));
	assert(IssueById(healthRegistry, compatId).at("context").value("installedVersion", "") == "1.5.0");
	const auto pre2Id = "compat.pre-2-view:view:legacy.mod/panel";
	assert(healthRegistry.IsActive(pre2Id));
	assert(IssueById(healthRegistry, pre2Id).value("severity", "") == "warning");
	assert(IssueById(healthRegistry, pre2Id).at("context").value("consumer", "") == "legacy.mod/panel");
	assert(IssueById(healthRegistry, pre2Id).at("context").value("targetVersion", "") == "1.9.0");
	assert(IssueById(healthRegistry, pre2Id).at("context").value("installedVersion", "") == "1.5.0");
	assert(IssueById(healthRegistry, pre2Id).at("context").value("removalVersion", "") == "2.1.0");

	const std::array legacyConsumers{
		CompatibilityTarget{ "SuitProtocol.dll", "plugin", "1.7", "compat.legacy-api",
			HealthRegistry::Severity::Warning, "2.1.0", "abi" },
		CompatibilityTarget{ "ak.autosort", "Papyrus mod", "1.x natives",
			"compat.legacy-papyrus", HealthRegistry::Severity::Warning, "2.1.0", "api" },
		CompatibilityTarget{ "FuturePlugin.dll", "plugin", "3.0", "compat.unsupported-api",
			HealthRegistry::Severity::Error, "", "abi" },
	};
	reconciler.SyncCompatibility(healthRegistry, legacyConsumers, "2.0.0", 3.5);
	const auto abi = IssueById(healthRegistry, "compat.legacy-api:plugin:SuitProtocol.dll");
	assert(abi.value("severity", "") == "warning");
	assert(abi.at("context").value("consumer", "") == "SuitProtocol.dll");
	assert(abi.at("context").value("abi", "") == "1.7");
	assert(abi.at("context").value("installedVersion", "") == "2.0.0");
	assert(abi.at("context").value("removalVersion", "") == "2.1.0");
	const auto papyrus = IssueById(healthRegistry, "compat.legacy-papyrus:Papyrus mod:ak.autosort");
	assert(papyrus.at("context").value("api", "") == "1.x natives");
	const auto unsupported = IssueById(healthRegistry, "compat.unsupported-api:plugin:FuturePlugin.dll");
	assert(unsupported.value("severity", "") == "error");
	assert(!unsupported.at("context").contains("removalVersion"));
	reconciler.SyncCompatibility(healthRegistry, targets, "1.5.0", 3.75);
	const auto occurrences = IssueById(healthRegistry, compatId).value("occurrences", 0u);
	const std::array reordered{ targets[2], targets[1], targets[0] };
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

	std::cout << "runtime health tests passed\n";
}
