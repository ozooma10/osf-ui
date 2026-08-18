#include "Runtime/RuntimeHealthCoordinator.h"

#include "API/BridgeApi.h"
#include "Compat/V1/Navigation.h"
#include "Compat/V1/Papyrus.h"
#include "Core/Version.h"
#include "Core/Json.h"
#include "Runtime/Runtime.h"

namespace OSFUI
{
	namespace
	{
		constexpr double kPollSeconds{ 2.0 };
	}

	void RuntimeHealthCoordinator::Pump()
	{
		auto& runtime = _runtime;
		if (!runtime._healthRegistry) return;

		DrainPluginHealthReports();
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
		runtime._healthRegistry->Broadcast();
	}

	void RuntimeHealthCoordinator::DrainPluginHealthReports()
	{
		auto& runtime = _runtime;
		const auto ops = API::BridgeApi::Get().TakeHealthIssueOps();
		const auto qualify = [](const std::string& a_modId, const std::string& a_local) {
			return a_modId + ":" + a_local;
		};
		for (const auto& op : ops) {
			switch (op.kind) {
			case API::BridgeApi::HealthIssueOp::Kind::kReport:
				runtime._healthRegistry->Upsert(HealthRegistry::IssueSpec{
					.id = qualify(op.modId, op.id),
					.code = qualify(op.modId, op.code),
					.severity = op.error ? HealthRegistry::Severity::Error : HealthRegistry::Severity::Warning,
					.source = op.modId,
					.sourceKind = HealthRegistry::SourceKind::Mod,
					.subject = op.subject,
					.context = op.context,
				}, runtime._uptime);
				break;
			case API::BridgeApi::HealthIssueOp::Kind::kClear:
				runtime._healthRegistry->Resolve(qualify(op.modId, op.id), runtime._uptime);
				break;
			case API::BridgeApi::HealthIssueOp::Kind::kClearExcept:
				{
					std::unordered_set<std::string> keep;
					keep.reserve(op.keep.size());
					for (const auto& local : op.keep) keep.insert(qualify(op.modId, local));
					runtime._healthRegistry->ResolveMissing(op.modId, keep, runtime._uptime);
				}
				break;
			}
		}
	}

	void RuntimeHealthCoordinator::SyncSettings()
	{
		auto& runtime = _runtime;
		if (!runtime._settings || !runtime._healthRegistry) return;
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
		_healthReconciler.SyncSettings(*runtime._healthRegistry, errors, runtime._uptime);
	}

	std::string RuntimeHealthCoordinator::HotkeyTargetId(std::string_view a_mod, std::string_view a_key)
	{
		return "settings.hotkey-target:" + std::string(a_mod) + "." + std::string(a_key);
	}

	void RuntimeHealthCoordinator::ReportHotkeyTargetFailure(std::string_view a_mod, std::string_view a_key, std::string_view a_script, std::string_view a_function, std::string_view a_message)
	{
		const auto id = HotkeyTargetId(a_mod, a_key);
		HotkeyTargetFailure failure{
			std::string(a_mod), std::string(a_key), std::string(a_script),
			std::string(a_function), std::string(a_message)
		};
		if (const auto it = _hotkeyTargetFailures.find(id); it != _hotkeyTargetFailures.end() && it->second.script == failure.script && it->second.function == failure.function && it->second.message == failure.message) {
			return;
		}
		_hotkeyTargetFailures.insert_or_assign(id, failure);
		REX::ERROR("Runtime: [content] declarative hotkey {}.{} could not queue {}.{} — {}", a_mod, a_key, a_script, a_function, a_message);
		SyncSettings();
		if (_runtime._healthRegistry) {
			_runtime._healthRegistry->Broadcast();
		}
	}

	void RuntimeHealthCoordinator::ResolveHotkeyTarget(std::string_view a_mod, std::string_view a_key)
	{
		if (_hotkeyTargetFailures.erase(HotkeyTargetId(a_mod, a_key)) == 0) {
			return;
		}
		SyncSettings();
		if (_runtime._healthRegistry) {
			_runtime._healthRegistry->Broadcast();
		}
	}

	void RuntimeHealthCoordinator::SyncCompatibility()
	{
		auto& runtime = _runtime;
		if (!runtime._healthRegistry) return;

		std::vector<CompatibilityTarget> targets;
		for (const auto& manifest : runtime._views.All()) {
			if (IsPre2Target(manifest.targetVersion)) {
				targets.push_back({ manifest.id, "view", manifest.targetVersion, "compat.pre-2-view", HealthRegistry::Severity::Warning, std::string(Compat::V1::kRemovalVersion) });
			} else if (IsTargetNewerThanInstalledRelease(manifest.targetVersion)) {
				targets.push_back({ manifest.id, "view", manifest.targetVersion });
			}
		}

		for (const auto& caller : API::BridgeApi::Get().TakeLegacyApiCallers()) {
			if (_legacyApiCallers.size() >= API::BridgeApi::kMaxLegacyCallers) {
				break;
			}
			const auto known = std::ranges::any_of(_legacyApiCallers,
				[&](const auto& seen) {
					return seen.module == caller.module && seen.supported == caller.supported;
				});
			if (!known) {
				_legacyApiCallers.push_back(caller);
			}
		}

		for (const auto& caller : _legacyApiCallers) {
			targets.push_back({
				caller.module.empty() ? std::string("(unidentified plugin)") : caller.module,
				"plugin",
				std::format("{}.{}", caller.major, caller.minor),
				caller.supported ? "compat.legacy-api" : "compat.unsupported-api",
				caller.supported ? HealthRegistry::Severity::Warning : HealthRegistry::Severity::Error,
				caller.supported ? std::string(Compat::V1::kRemovalVersion) : std::string{},
				"abi",
			});
		}
		for (auto& caller : Compat::V1::Papyrus::TakeCallers()) {
			_legacyPapyrusCallers.insert(std::move(caller));
		}
		for (const auto& mod : _legacyPapyrusCallers) {
			targets.push_back({ mod, "Papyrus mod", "1.x natives", "compat.legacy-papyrus", HealthRegistry::Severity::Warning, std::string(Compat::V1::kRemovalVersion), "api" });
		}
		if (runtime._settings) {
			const auto data = runtime._settings->Store().DataView();
			if (const auto* mods = Json::GetArray(data, "mods")) {
				for (const auto& mod : *mods) {
					const auto target = Json::Get(mod, "targetVersion", "");
					if (IsTargetNewerThanInstalledRelease(target)) {
						targets.push_back({ Json::Get(mod, "id", ""), "mod", target });
					}
				}
			}
		}
		for (const auto& target : targets) {
			if (target.removalVersion.empty() && target.code != "compat.unsupported-api") continue;
			const auto identity = target.code + ':' + target.kind + ':' + target.id;
			if (_loggedCompatibility.insert(identity).second) {
				if (target.code == "compat.unsupported-api") {
					REX::WARN("Compatibility: {} '{}' requested unsupported ABI {}; OSF UI {} refused it", target.kind, target.id, target.targetVersion, kOsfuiReleaseVersion);
				} else {
					REX::WARN("Compatibility: {} '{}' targets {}; OSF UI {} kept it running via the temporary 1.x bridge, which will be removed in {}", target.kind, target.id, target.targetVersion, kOsfuiReleaseVersion, target.removalVersion);
				}
			}
		}
		_healthReconciler.SyncCompatibility(*runtime._healthRegistry, targets, kOsfuiReleaseVersion, runtime._uptime);
	}

	void RuntimeHealthCoordinator::UpdateSystemInfo()
	{
		auto& runtime = _runtime;
		if (!runtime._healthRegistry) return;
		const auto status = runtime._compositor ?
			runtime._compositor->GetStatus() : CompositorStatus{};
		runtime._healthRegistry->SetSystemInfo(nlohmann::json{
			{ "version", kOsfuiReleaseVersion },
			{ "bridgeVersion", kBridgeProtocolVersion },
			{ "renderer", runtime._renderer ? "webview2" : "none" },
			{ "compositor", runtime._compositor ? "d3d12" : "none" },
			{ "drawPath", runtime.OverlayCanDraw() ? "ui-pass" : "unavailable" },
			{ "frameGeneration", status.frameGeneration },
			{ "nativeFocus", runtime._renderer != nullptr },
			{ "locale", runtime._localization.Locale() },
			// { "devMode", runtime._config.devMode },
		});
	}

	void RuntimeHealthCoordinator::OnRendererHealth(const WebView2HostWebRenderer::HealthEvent& a_event)
	{
		auto& runtime = _runtime;
		if (!runtime._healthRegistry || a_event.code.empty()) return;
		const std::string code(a_event.code);
		if (!a_event.active) {
			runtime._healthRegistry->Resolve(code, runtime._uptime);
			runtime._healthRegistry->Broadcast();
			return;
		}
		nlohmann::json context = nlohmann::json::object();
		if (!a_event.detail.empty()) context["detail"] = std::string(a_event.detail);
		context["renderer"] = runtime._renderer ? "webview2" : "none";
		runtime._healthRegistry->Upsert(HealthRegistry::IssueSpec{
			.id = code,
			.code = code,
			.severity = HealthRegistry::Severity::Warning,
			.source = "host",
			.subject = runtime._renderer ? "webview2" : std::string{},
			.context = std::move(context),
		}, runtime._uptime);
		runtime._healthRegistry->Broadcast();
	}


	void RuntimeHealthCoordinator::ReportViewLoad(std::string_view a_viewId, bool a_failed,
		std::string_view a_description, int a_errorCode, std::uint32_t a_attemptsLeft)
	{
		auto& runtime = _runtime;
		if (!runtime._healthRegistry) return;
		_healthReconciler.ReportViewLoad(*runtime._healthRegistry, a_viewId, a_failed,
			a_description, a_errorCode, a_attemptsLeft, runtime._uptime);
	}
}
