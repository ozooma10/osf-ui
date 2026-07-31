#include "runtime/DiagnosticsReconciler.h"

#include "core/Log.h"

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
	nlohmann::json IssueById(const OSFUI::DiagnosticsModule& a_diagnostics,
		std::string_view a_id)
	{
		const auto snapshot = a_diagnostics.Snapshot();
		for (const auto& issue : snapshot.at("issues")) {
			if (issue.value("id", "") == a_id) return issue;
		}
		return {};
	}
}

int main()
{
	using namespace OSFUI;
	DiagnosticsModule diagnostics;
	DiagnosticsReconciler reconciler;

	const std::array settingsErrors{
		SettingsLoadIssue{ "values-parse", "values.json", "acme", "bad value" },
		SettingsLoadIssue{ "schema-parse", "schema.json", "beta", "bad schema" },
		SettingsLoadIssue{ "hotkey-target", "acme.mod.json", "acme.mod.startScene", "missing target",
			nlohmann::json{ { "script", "Acme_Hotkeys" }, { "function", "OnHotkey" } } },
	};
	reconciler.SyncSettings(diagnostics, settingsErrors, 1.0);
	assert(diagnostics.IsActive("settings.values-parse:acme"));
	assert(diagnostics.IsActive("settings.schema-parse:beta"));
	assert(IssueById(diagnostics, "settings.values-parse:acme").value("severity", "") == "warning");
	assert(IssueById(diagnostics, "settings.schema-parse:beta").value("severity", "") == "error");
	const auto hotkeyId = "settings.hotkey-target:acme.mod.startScene";
	assert(diagnostics.IsActive(hotkeyId));
	assert(IssueById(diagnostics, hotkeyId).at("context").value("script", "") == "Acme_Hotkeys");
	const auto hotkeyOccurrences = IssueById(diagnostics, hotkeyId).value("occurrences", 0u);
	reconciler.SyncSettings(diagnostics, settingsErrors, 1.5);
	assert(IssueById(diagnostics, hotkeyId).value("occurrences", 0u) == hotkeyOccurrences);
	reconciler.SyncSettings(diagnostics, {}, 2.0);
	assert(!diagnostics.IsActive("settings.values-parse:acme"));
	assert(!diagnostics.IsActive("settings.schema-parse:beta"));
	assert(!diagnostics.IsActive(hotkeyId));

	const std::array targets{
		CompatibilityTarget{ "beta/mod", "mod", "2.0.0" },
		CompatibilityTarget{ "acme/view", "view", "2.0.0" },
	};
	reconciler.SyncCompatibility(diagnostics, targets, "1.5.0", 3.0);
	const auto compatId = "compat.needs-newer-osfui:view:acme/view";
	assert(diagnostics.IsActive(compatId));
	assert(IssueById(diagnostics, compatId).at("context").value("installedVersion", "") == "1.5.0");
	const auto occurrences = IssueById(diagnostics, compatId).value("occurrences", 0u);
	const std::array reordered{ targets[1], targets[0] };
	reconciler.SyncCompatibility(diagnostics, reordered, "1.5.0", 4.0);
	assert(IssueById(diagnostics, compatId).value("occurrences", 0u) == occurrences);
	reconciler.SyncCompatibility(diagnostics,
		std::span<const CompatibilityTarget>{ targets.data(), 1 }, "1.5.0", 5.0);
	assert(!diagnostics.IsActive(compatId));

	reconciler.ReportViewLoad(diagnostics, "acme/view", true, "timeout", -1, 2, 6.0);
	assert(diagnostics.IsActive("view.load-retrying:acme/view"));
	reconciler.ReportViewLoad(diagnostics, "acme/view", true, "timeout", -1, 0, 7.0);
	assert(!diagnostics.IsActive("view.load-retrying:acme/view"));
	assert(diagnostics.IsActive("view.load-failed:acme/view"));
	reconciler.ReportViewLoad(diagnostics, "acme/view", false, "", 0, 0, 8.0);
	assert(!diagnostics.IsActive("view.load-failed:acme/view"));

	std::cout << "runtime diagnostics tests passed\n";
}
