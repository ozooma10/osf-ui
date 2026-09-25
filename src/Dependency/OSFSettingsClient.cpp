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
		const bool settingsAvailable = m_settings.Init();
		const bool diagnosticsAvailable = m_diagnostics.Init();
		m_available = settingsAvailable && m_settings.IsReady() && diagnosticsAvailable;
		if (!m_available) {
			REX::ERROR("OSF UI requires ready OSF Settings Slim services (settings ABI 1.0 and diagnostics ABI 1.0); settings={}, ready={}, diagnostics={}", settingsAvailable, m_settings.IsReady(), diagnosticsAvailable);
			ReportFailure("dependency", "dependency.settings-unavailable", "OSF UI cannot start without compatible OSF Settings services");
			return false;
		}
		ReadStartupBool("developerMode", m_developerMode);
		ReadStartupBool("highRefreshCapture", m_highRefreshCapture);
		ClearFailure("dependency");
		return true;
	}

	void OSFSettingsClient::RegisterLaunchers(std::span<const ViewManifest> a_views)
	{
		OSFSettings::API::Launcher::Client launcher;
		if (!launcher.Init()) return; // Optional; direct view opens still work with older Settings.
		for (const auto& view : a_views) {
			if (view.kind != ViewKind::Menu || view.launcherMod.empty() || (view.debugOnly && !m_developerMode)) continue;
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
		if (!m_language.empty() || !m_available || !m_settings.Has(OSFSettings::API::kLanguageVersion)) {
			return m_language;
		}
		std::string language;
		const auto status = m_settings.GetLanguage(language);
		if (status == Status::NotReady) {
			return m_language;  // translations not loaded yet; ask again later
		}
		if (Check(status, "read language")) {
			m_language = std::move(language);
			REX::INFO("OSF Settings reports game language '{}'", m_language);
		}
		return m_language;
	}

	void OSFSettingsClient::ReadStartupBool(const char* a_key, bool& a_value)
	{
		a_value = false;
		const auto status = m_settings.GetBool("osfui", a_key, &a_value);
		if (status == Status::Ok) return;
		a_value = false;
		REX::WARN("OSF Settings read failed for osfui/{}: status {}; using false until restart", a_key, static_cast<unsigned>(status));
		IssueText issue{ Severity::Warning, "An OSF UI startup setting could not be read",
			std::format("{} is disabled for this session.", a_key),
			"Reinstall OSF UI's osfui.json settings schema and restart Starfield." };
		const auto id = std::string("settings.") + a_key;
		Report(id, issue);
	}

	bool OSFSettingsClient::Check(Status a_status, std::string a_operation)
	{
		if (a_status == Status::Ok) {
			m_failedOperations.erase(a_operation);
			return true;
		}
		if (m_failedOperations.insert(a_operation).second) {
			REX::WARN("OSF Settings {} failed: status {}; will retry", a_operation, static_cast<unsigned>(a_status));
		}
		return false;
	}

	void OSFSettingsClient::Report(std::string_view a_id, const IssueText& a_issue)
	{
		if (!m_diagnostics) return;
		const auto id = std::string(a_id);
		const OSFSettings::API::Diagnostics::Issue issue{
			.modId = "osfui", .id = id.c_str(), .severity = a_issue.severity,
			.title = a_issue.title.c_str(), .impact = a_issue.impact.c_str(), .nextSteps = a_issue.nextSteps.c_str()
		};
		m_diagnostics.Report(issue);
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
		Report(a_id, issue);
	}

	void OSFSettingsClient::ClearFailure(std::string_view a_id)
	{
		if (!m_diagnostics) return;
		const auto id = std::string(a_id);
		m_diagnostics.Clear("osfui", id.c_str());
	}

	bool OSFSettingsClient::AcquireInputSuppression()
	{
		if (m_hotkeyBlock) return true;
		if (!m_available) return false;
		OSFSettings::API::HotkeyBlock block{};
		const auto status = m_settings.AcquireHotkeyBlock(&block);
		if (!Check(status, "acquire hotkey block") || !block) return false;
		m_hotkeyBlock = block;
		ClearFailure("input.hotkey-block");
		return true;
	}

	bool OSFSettingsClient::ReleaseInputSuppression()
	{
		if (!m_hotkeyBlock) return true;
		const auto status = m_settings.ReleaseHotkeyBlock(m_hotkeyBlock);
		if (status != Status::UnknownHotkeyBlock && !Check(status, "release hotkey block")) return false;
		m_hotkeyBlock = 0;
		m_failedOperations.erase("release hotkey block");
		return true;
	}
}
