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

		std::unordered_set<std::string> live;
		for (const auto& error : runtime._settings->Store().LoadErrors()) {
			const auto subject = error.mod.empty() ? error.file : error.mod;
			auto id = "settings." + error.kind + ":" + subject;
			live.insert(id);
			runtime._diagnostics->Upsert(DiagnosticsModule::IssueSpec{
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
			}, runtime._uptime);
		}
		runtime._diagnostics->ResolveMissing("settings", live, runtime._uptime);
	}

	void RuntimeDiagnostics::SyncCompatibility()
	{
		auto& runtime = _runtime;
		if (!runtime._diagnostics) return;

		struct Wanting
		{
			std::string id;
			std::string kind;
			std::string target;
		};
		std::vector<Wanting> wanting;
		for (const auto& manifest : runtime._views.All()) {
			if (IsTargetNewerThanHost(manifest.targetVersion)) {
				wanting.push_back({ manifest.id, "view", manifest.targetVersion });
			}
		}
		if (runtime._settings) {
			for (const auto& mod : runtime._settings->Store().DataView().value(
					"mods", nlohmann::json::array())) {
				const auto target = mod.value("targetVersion", std::string{});
				if (IsTargetNewerThanHost(target)) {
					wanting.push_back({ mod.value("id", std::string{}), "mod", target });
				}
			}
		}

		std::string signature;
		for (const auto& item : wanting) {
			signature += item.kind + ':' + item.id + '@' + item.target + ';';
		}
		if (signature == _compatSignature) return;
		_compatSignature = std::move(signature);

		std::unordered_set<std::string> live;
		for (const auto& item : wanting) {
			auto id = "compat.needs-newer-osfui:" + item.kind + ':' + item.id;
			live.insert(id);
			runtime._diagnostics->Upsert(DiagnosticsModule::IssueSpec{
				.id = std::move(id),
				.code = "compat.needs-newer-osfui",
				.severity = DiagnosticsModule::Severity::Warning,
				.source = "compat",
				.subject = item.id,
				.context = nlohmann::json{
					{ "kind", item.kind },
					{ "targetVersion", item.target },
					{ "installedVersion", kPluginVersion },
				},
			}, runtime._uptime);
		}
		runtime._diagnostics->ResolveMissing("compat", live, runtime._uptime);
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
			{ "debugMode", runtime._config.debugMode },
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

#if defined(OSFUI_WITH_WORLD_SURFACES)
	void RuntimeDiagnostics::OnWorldSurfaceHealth(
		std::size_t a_index, const IWebRenderer::HealthEvent& a_event)
	{
		auto& runtime = _runtime;
		if (!runtime._diagnostics || a_event.code.empty()) return;
		const auto id = std::format("world{}:{}", a_index + 1, a_event.code);
		if (!a_event.active) {
			runtime._diagnostics->Resolve(id, runtime._uptime);
			runtime._diagnostics->Broadcast();
			return;
		}
		nlohmann::json context = nlohmann::json::object();
		if (!a_event.detail.empty()) context["detail"] = std::string(a_event.detail);
		context["surface"] = a_index < runtime._worldSurfaces.size() ?
			runtime._worldSurfaces[a_index].viewId : std::string{};
		runtime._diagnostics->Upsert(DiagnosticsModule::IssueSpec{
			.id = id,
			.code = std::string(a_event.code),
			.severity = DiagnosticsModule::Severity::Warning,
			.source = "host",
			.subject = std::format("world{}", a_index + 1),
			.context = std::move(context),
		}, runtime._uptime);
		runtime._diagnostics->Broadcast();
	}
#endif

	void RuntimeDiagnostics::ReportViewLoad(std::string_view a_viewId, bool a_failed,
		std::string_view a_description, int a_errorCode, std::uint32_t a_attemptsLeft)
	{
		auto& runtime = _runtime;
		if (!runtime._diagnostics) return;
		const std::string id(a_viewId);
		const auto retrying = "view.load-retrying:" + id;
		const auto failed = "view.load-failed:" + id;
		if (!a_failed) {
			runtime._diagnostics->Resolve(retrying, runtime._uptime);
			runtime._diagnostics->Resolve(failed, runtime._uptime);
			return;
		}
		nlohmann::json context{
			{ "errorCode", a_errorCode },
			{ "description", std::string(a_description) },
			{ "attemptsLeft", a_attemptsLeft },
		};
		if (a_attemptsLeft > 0) {
			runtime._diagnostics->Upsert(DiagnosticsModule::IssueSpec{
				.id = retrying,
				.code = "view.load-retrying",
				.severity = DiagnosticsModule::Severity::Warning,
				.source = "views",
				.subject = id,
				.context = std::move(context),
			}, runtime._uptime);
			return;
		}
		runtime._diagnostics->Resolve(retrying, runtime._uptime);
		runtime._diagnostics->Upsert(DiagnosticsModule::IssueSpec{
			.id = failed,
			.code = "view.load-failed",
			.severity = DiagnosticsModule::Severity::Error,
			.source = "views",
			.subject = id,
			.context = std::move(context),
		}, runtime._uptime);
	}
}