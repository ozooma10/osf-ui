#include "Dependency/OSFSettingsClient.h"

#include "Core/Json.h"
#include "API/BridgeApi.h"
#include "Views/ViewManifest.h"
#include "vendor/OSFSettings_Launcher.h"

namespace OSFUI
{
	namespace
	{
		using Status = OSFSettings::API::Status;
		using Severity = OSFSettings::API::Diagnostics::Severity;
		constexpr const char* kRepair = "Check the mod installation and the OSF UI log, then restart Starfield.";
	}

	bool OSFSettingsClient::Initialize()
	{
		const bool settingsAvailable = _settings.Init();
		const bool diagnosticsAvailable = _diagnostics.Init();
		_available = settingsAvailable && _settings.IsReady() && diagnosticsAvailable;
		if (!_available) {
			REX::ERROR("OSF UI requires ready OSF Settings Slim services (settings ABI 1.0 and diagnostics ABI 1.0); settings={}, ready={}, diagnostics={}",
				settingsAvailable, _settings.IsReady(), diagnosticsAvailable);
			ReportFailure("dependency", "dependency.settings-unavailable", "OSF UI cannot start without compatible OSF Settings services");
			return false;
		}
		ReadStartupBool("developerMode", _developerMode);
		ReadStartupBool("highRefreshCapture", _highRefreshCapture);
		ClearFailure("dependency");
		return true;
	}

	void OSFSettingsClient::RegisterLaunchers(std::span<const ViewManifest> a_views)
	{
		OSFSettings::API::Launcher::Client launcher;
		if (!launcher.Init()) return; // Optional; direct view opens still work with older Settings.
		for (const auto& view : a_views) {
			if (view.kind != ViewKind::Menu || view.launcherMod.empty() || (view.debugOnly && !_developerMode)) continue;
			const auto result = launcher.Register({
				.modId = view.launcherMod.c_str(), .id = view.id.c_str(), .modTitle = view.launcherModTitle.c_str(),
				.title = view.title.c_str(), .description = view.description.c_str(),
				.open = [](const char*, const char* id, void*) noexcept {
					if (!API::BridgeApi::Get().RequestMenu(id, true)) REX::WARN("Launcher could not queue view '{}'", id);
				}
			});
			if (result != OSFSettings::API::Launcher::Status::Ok)
				REX::WARN("Launcher registration for '{}' failed: {}", view.id, static_cast<unsigned>(result));
		}
	}

	std::string OSFSettingsClient::Language()
	{
		if (!_language.empty() || !_available || !_settings.Has(OSFSettings::API::kLanguageVersion)) {
			return _language;
		}
		std::string language;
		const auto status = _settings.GetLanguage(language);
		if (status == Status::NotReady) {
			return _language;  // translations not loaded yet; ask again later
		}
		if (Check(status, "read language")) {
			_language = std::move(language);
			REX::INFO("OSF Settings reports game language '{}'", _language);
		}
		return _language;
	}

	void OSFSettingsClient::ReadStartupBool(const char* a_key, bool& a_value)
	{
		a_value = false;
		const auto status = _settings.GetBool("osfui", a_key, &a_value);
		if (status == Status::Ok) return;
		a_value = false;
		REX::WARN("OSF Settings read failed for osfui/{}: status {}; using false until restart", a_key, static_cast<unsigned>(status));
		IssueText issue{ Severity::Warning, "An OSF UI startup setting could not be read",
			std::format("{} is disabled for this session.", a_key),
			"Reinstall OSF UI's osfui.json settings schema and restart Starfield." };
		const auto id = std::string("settings.") + a_key;
		_desired.insert_or_assign(id, issue);
		Report(id, issue);
	}

	bool OSFSettingsClient::Check(Status a_status, std::string a_operation)
	{
		if (a_status == Status::Ok) {
			_failedOperations.erase(a_operation);
			return true;
		}
		if (_failedOperations.insert(a_operation).second) {
			REX::WARN("OSF Settings {} failed: status {}; will retry", a_operation, static_cast<unsigned>(a_status));
		}
		return false;
	}

	void OSFSettingsClient::Report(std::string_view a_id, const IssueText& a_issue)
	{
		if (!_diagnostics) return;
		const auto id = std::string(a_id);
		const auto existing = _reported.find(id);
		if (existing != _reported.end() && existing->second == a_issue) return;
		const OSFSettings::API::Diagnostics::Issue issue{
			.modId = "osfui", .id = id.c_str(), .severity = a_issue.severity,
			.title = a_issue.title.c_str(), .impact = a_issue.impact.c_str(), .nextSteps = a_issue.nextSteps.c_str()
		};
		if (Check(_diagnostics.Report(issue), "report " + id)) _reported.insert_or_assign(id, a_issue);
	}

	bool OSFSettingsClient::Clear(std::string_view a_id)
	{
		const auto id = std::string(a_id);
		_failedOperations.erase("report " + id);
		if (!_reported.contains(id)) return true;
		if (!Check(_diagnostics.Clear("osfui", id.c_str()), "clear " + id)) return false;
		_reported.erase(id);
		return true;
	}

	OSFSettingsClient::IssueText OSFSettingsClient::Describe(std::string_view a_code, std::string_view a_message, const nlohmann::json& a_context)
	{
		IssueText result{ Severity::Error, std::string(a_message), {}, kRepair };
		const auto view = Json::Get(a_context, "view", "");
		if (!view.empty()) result.impact = std::format("The view '{}' is unavailable.", view);
		const auto degraded = std::format("The view '{}' may be unavailable or degraded.", view);
		if (a_code == "view.load-retrying") {
			result = { Severity::Warning, "A mod WebView is taking longer to load", degraded,
				"OSF UI is retrying automatically. Check the mod installation if the problem persists." };
		} else if (a_code == "view.load-failed") {
			result.title = "A mod WebView could not load";
		} else if (a_code == "view.protocol-misuse") {
			result = { Severity::Warning, "A mod WebView is sending unsupported requests", degraded,
				"Update the owning mod and OSF UI to compatible versions." };
		}
		return result;
	}

	void OSFSettingsClient::ReportFailure(std::string_view a_id, std::string_view a_code,
		std::string_view a_message, const nlohmann::json& a_context)
	{
		const auto issue = Describe(a_code, a_message, a_context);
		if (issue.severity == Severity::Error) {
			REX::ERROR("OSF UI failure {} [{}]: {} {}", a_id, a_code, a_message, Json::Dump(a_context));
		} else {
			REX::WARN("OSF UI health {} [{}]: {} {}", a_id, a_code, a_message, Json::Dump(a_context));
		}
		const auto id = std::string(a_id);
		_desired.insert_or_assign(id, issue);
		Report(id, issue);
	}

	void OSFSettingsClient::ClearFailure(std::string_view a_id)
	{
		_desired.erase(std::string(a_id));
		Clear(a_id);
	}

	void OSFSettingsClient::RetryDiagnostics()
	{
		if (!_diagnostics) return;
		for (const auto& [id, issue] : _desired) Report(id, issue);
		// Copy IDs before erasing; a failed clear must remain cached for a later retry.
		std::vector<std::string> stale;
		for (const auto& [id, issue] : _reported) if (!_desired.contains(id)) stale.push_back(id);
		for (const auto& id : stale) Clear(id);
	}

	bool OSFSettingsClient::AcquireInputSuppression()
	{
		if (_hotkeyBlock) return true;
		if (!_available) return false;
		OSFSettings::API::HotkeyBlock block{};
		const auto status = _settings.AcquireHotkeyBlock(&block);
		if (!Check(status, "acquire hotkey block") || !block) return false;
		_hotkeyBlock = block;
		ClearFailure("input.hotkey-block");
		return true;
	}

	bool OSFSettingsClient::ReleaseInputSuppression()
	{
		if (!_hotkeyBlock) return true;
		const auto status = _settings.ReleaseHotkeyBlock(_hotkeyBlock);
		if (status != Status::UnknownHotkeyBlock && !Check(status, "release hotkey block")) return false;
		_hotkeyBlock = 0;
		_failedOperations.erase("release hotkey block");
		return true;
	}
}
