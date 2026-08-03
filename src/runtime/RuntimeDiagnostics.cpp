#include "runtime/RuntimeDiagnostics.h"

#include "api/BridgeApi.h"
#include "core/Version.h"
#include "runtime/Runtime.h"

namespace OSFUI
{
	namespace
	{
		constexpr double kPollSeconds{ 2.0 };
	}

	void RuntimeDiagnostics::Pump()
	{
		auto& runtime = _runtime;
		if (!runtime._diagnostics) return;

		DrainPluginReports();
		if (runtime._settings) {
			const auto generation = runtime._settings->Store().Generation();
			if (!_settingsSynced || generation != _settingsGeneration) {
				_settingsGeneration = generation;
				_settingsSynced = true;
				SyncSettings();
			}
		}
		if (runtime._uptime >= _nextPoll) {
			_nextPoll = runtime._uptime + kPollSeconds;
			SyncCompatibility();
			UpdateSystemInfo();
		}
		runtime._diagnostics->Broadcast();
	}

	void RuntimeDiagnostics::DrainPluginReports()
	{
		auto& runtime = _runtime;
		const auto ops = API::BridgeApi::Get().TakeDiagnosticOps();
		const auto qualify = [](const std::string& a_modId, const std::string& a_local) {
			return a_modId + ":" + a_local;
		};
		for (const auto& op : ops) {
			switch (op.kind) {
			case API::BridgeApi::DiagnosticOp::Kind::kReport:
				runtime._diagnostics->Upsert(DiagnosticsModule::IssueSpec{
					.id = qualify(op.modId, op.id),
					.code = qualify(op.modId, op.code),
					.severity = op.error ? DiagnosticsModule::Severity::Error :
						DiagnosticsModule::Severity::Warning,
					.source = op.modId,
					.subject = op.subject,
					.context = op.context,
				}, runtime._uptime);
				break;
			case API::BridgeApi::DiagnosticOp::Kind::kClear:
				runtime._diagnostics->Resolve(qualify(op.modId, op.id), runtime._uptime);
				break;
			case API::BridgeApi::DiagnosticOp::Kind::kClearExcept:
				{
					std::unordered_set<std::string> keep;
					keep.reserve(op.keep.size());
					for (const auto& local : op.keep) keep.insert(qualify(op.modId, local));
					runtime._diagnostics->ResolveMissing(op.modId, keep, runtime._uptime);
				}
				break;
			}
		}
	}

	void RuntimeDiagnostics::SyncSettings()
	{
		auto& runtime = _runtime;
		if (!runtime._settings || !runtime._diagnostics) return;
		auto& store = runtime._settings->Store();
		for (auto it = _hotkeyTargetFailures.begin(); it != _hotkeyTargetFailures.end();) {
			const auto target = store.GetHotkeyTarget(it->second.mod, it->second.key);
			if (!target || target->script != it->second.script || target->function != it->second.function) {
				it = _hotkeyTargetFailures.erase(it);
			} else {
				++it;
			}
		}

		std::vector<SettingsLoadIssue> errors;
		for (const auto& error : store.LoadErrors()) {
			errors.push_back({ error.kind, error.file, error.mod, error.message });
		}
		for (const auto& issue : store.HotkeyTargetIssues()) {
			errors.push_back({ "hotkey-target", issue.file, issue.mod + "." + issue.key,
				issue.message, nlohmann::json::object() });
		}
		for (const auto& [id, failure] : _hotkeyTargetFailures) {
			(void)id;
			errors.push_back({ "hotkey-target", "", failure.mod + "." + failure.key,
				failure.message, nlohmann::json{
					{ "script", failure.script },
					{ "function", failure.function },
				} });
		}
		_reconciler.SyncSettings(*runtime._diagnostics, errors, runtime._uptime);
	}

	std::string RuntimeDiagnostics::HotkeyTargetId(std::string_view a_mod, std::string_view a_key)
	{
		return "settings.hotkey-target:" + std::string(a_mod) + "." + std::string(a_key);
	}

	void RuntimeDiagnostics::ReportHotkeyTargetFailure(std::string_view a_mod, std::string_view a_key,
		std::string_view a_script, std::string_view a_function, std::string_view a_message)
	{
		const auto id = HotkeyTargetId(a_mod, a_key);
		HotkeyTargetFailure failure{
			std::string(a_mod), std::string(a_key), std::string(a_script),
			std::string(a_function), std::string(a_message)
		};
		if (const auto it = _hotkeyTargetFailures.find(id); it != _hotkeyTargetFailures.end() &&
			it->second.script == failure.script && it->second.function == failure.function &&
			it->second.message == failure.message) {
			return;
		}
		_hotkeyTargetFailures.insert_or_assign(id, failure);
		REX::ERROR("Runtime: [content] declarative hotkey {}.{} could not queue {}.{} — {}",
			a_mod, a_key, a_script, a_function, a_message);
		SyncSettings();
		if (_runtime._diagnostics) {
			_runtime._diagnostics->Broadcast();
		}
	}

	void RuntimeDiagnostics::ResolveHotkeyTarget(std::string_view a_mod, std::string_view a_key)
	{
		if (_hotkeyTargetFailures.erase(HotkeyTargetId(a_mod, a_key)) == 0) {
			return;
		}
		SyncSettings();
		if (_runtime._diagnostics) {
			_runtime._diagnostics->Broadcast();
		}
	}

	void RuntimeDiagnostics::SyncCompatibility()
	{
		auto& runtime = _runtime;
		if (!runtime._diagnostics) return;

		std::vector<CompatibilityTarget> targets;
		for (const auto& manifest : runtime._views.All()) {
			if (IsTargetNewerThanHost(manifest.targetVersion)) {
				targets.push_back({ manifest.id, "view", manifest.targetVersion });
			}
		}
		// Plugins refused by OSFUI_RequestBridge for an ABI major mismatch. The
		// refusal happens during SFSE load, long before this runs, so the record
		// is drained here rather than reported at the refusal site.
		// Deduped and capped HERE, not just at the producer: the drain empties
		// BridgeApi's dedupe set, so a plugin that retries on every load screen
		// would otherwise add itself again on every poll for the whole session.
		for (const auto& caller : API::BridgeApi::Get().TakeLegacyApiCallers()) {
			if (_legacyApiCallers.size() >= API::BridgeApi::kMaxLegacyCallers) {
				break;
			}
			const auto known = std::ranges::any_of(_legacyApiCallers,
				[&](const auto& seen) { return seen.module == caller.module; });
			if (!known) {
				_legacyApiCallers.push_back(caller);
			}
		}
		for (const auto& caller : _legacyApiCallers) {
			targets.push_back({
				caller.module.empty() ? std::string("(unidentified plugin)") : caller.module,
				"plugin",
				std::format("{}.{}", caller.major, caller.minor),
				"compat.legacy-api",
			});
		}
		if (runtime._settings) {
			for (const auto& mod : runtime._settings->Store().DataView().value(
					"mods", nlohmann::json::array())) {
				const auto target = mod.value("targetVersion", std::string{});
				if (IsTargetNewerThanHost(target)) {
					targets.push_back({ mod.value("id", std::string{}), "mod", target });
				}
			}
		}
		_reconciler.SyncCompatibility(
			*runtime._diagnostics, targets, kPluginVersion, runtime._uptime);
	}

	void RuntimeDiagnostics::UpdateSystemInfo()
	{
		auto& runtime = _runtime;
		if (!runtime._diagnostics) return;
		const auto stats = runtime._compositor ?
			runtime._compositor->GetRenderStats() : CompositorStats{};
		runtime._diagnostics->SetSystemInfo(nlohmann::json{
			{ "version", kPluginVersion },
			{ "bridgeVersion", kBridgeProtocolVersion },
			{ "renderer", runtime._renderer ? std::string(runtime._renderer->Name()) : "none" },
			{ "compositor", runtime._compositor ? std::string(runtime._compositor->Name()) : "none" },
			{ "drawPath", stats.seamActive ? "ui-seam" : "unavailable" },
			{ "frameGeneration", stats.frameGeneration },
			{ "nativeFocus", runtime._renderer && runtime._renderer->UsesNativeKeyboardFocus() },
			{ "locale", runtime._localization.Locale() },
			{ "devMode", runtime._config.devMode },
		});
	}

	void RuntimeDiagnostics::OnRendererHealth(const IWebRenderer::HealthEvent& a_event)
	{
		auto& runtime = _runtime;
		if (!runtime._diagnostics || a_event.code.empty()) return;
		const std::string code(a_event.code);
		if (!a_event.active) {
			runtime._diagnostics->Resolve(code, runtime._uptime);
			runtime._diagnostics->Broadcast();
			return;
		}
		nlohmann::json context = nlohmann::json::object();
		if (!a_event.detail.empty()) context["detail"] = std::string(a_event.detail);
		context["renderer"] = runtime._renderer ? std::string(runtime._renderer->Name()) : "none";
		runtime._diagnostics->Upsert(DiagnosticsModule::IssueSpec{
			.id = code,
			.code = code,
			.severity = DiagnosticsModule::Severity::Warning,
			.source = "host",
			.subject = runtime._renderer ? std::string(runtime._renderer->Name()) : std::string{},
			.context = std::move(context),
		}, runtime._uptime);
		runtime._diagnostics->Broadcast();
	}


	void RuntimeDiagnostics::ReportViewLoad(std::string_view a_viewId, bool a_failed,
		std::string_view a_description, int a_errorCode, std::uint32_t a_attemptsLeft)
	{
		auto& runtime = _runtime;
		if (!runtime._diagnostics) return;
		_reconciler.ReportViewLoad(*runtime._diagnostics, a_viewId, a_failed,
			a_description, a_errorCode, a_attemptsLeft, runtime._uptime);
	}
}
