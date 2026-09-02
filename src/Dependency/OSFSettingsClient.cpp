#include "Dependency/OSFSettingsClient.h"

#include <iomanip>
#include <sstream>

#include "Core/Json.h"
#include "Core/Version.h"

namespace OSFUI
{
	bool OSFSettingsClient::Initialize()
	{
		_settingsAvailable = _settings.Init(OSFSettings::API::Settings::kVersion);
		_diagnosticsAvailable = _diagnostics.Init(OSFSettings::API::Diagnostics::kVersion);
		if (!_settingsAvailable) {
			REX::ERROR("OSF UI requires OSF Settings >=1.0.0 <2.0.0; compatible RequestSettings table was not found");
			if (_diagnosticsAvailable) ReportFailure("dependency", "dependency.settings-incompatible",
				"OSF UI requires OSF Settings >=1.0.0 <2.0.0");
			return false;
		}
		if (!_diagnosticsAvailable) {
			REX::ERROR("OSF UI requires the OSF Settings v1 Diagnostics table");
			return false;
		}

		OSFSettings::API::Diagnostics::ComponentInfo info;
		info.id = "osfui"; info.name = "OSF UI"; info.version = kOsfuiReleaseVersion; info.build = "webview-addon";
		_diagnostics.RegisterComponent(info);
		_settings.GetBool("osfui", "developerMode", &_developerMode);
		_settings.GetBool("osfui", "highRefreshCapture", &_highRefreshCapture);
		ClearFailure("dependency");
		return true;
	}

	std::string OSFSettingsClient::BoundedId(std::string_view a_id)
	{
		if (a_id.size() <= 64) return std::string(a_id);
		std::uint64_t hash = 14695981039346656037ull;
		for (const auto c : a_id) { hash ^= static_cast<unsigned char>(c); hash *= 1099511628211ull; }
		std::ostringstream suffix; suffix << std::hex << std::setw(16) << std::setfill('0') << hash;
		return std::string(a_id.substr(0, 47)) + "-" + suffix.str();
	}

	void OSFSettingsClient::ReportFailure(std::string_view a_id, std::string_view a_code,
		std::string_view a_message, const nlohmann::json& a_context)
	{
		if (!_diagnosticsAvailable) return;
		const auto id = BoundedId(a_id); auto context = Json::Dump(a_context);
		_directFailures.insert(id);
		_diagnostics.Report("osfui", id.c_str(), std::string(a_code).c_str(),
			OSFSettings::API::Diagnostics::Severity::kError, std::string(a_message).c_str(), context.c_str());
	}

	void OSFSettingsClient::ClearFailure(std::string_view a_id)
	{
		if (!_diagnosticsAvailable) return;
		const auto id = BoundedId(a_id);
		_directFailures.erase(id);
		_diagnostics.Clear("osfui", id.c_str());
	}

	void OSFSettingsClient::SyncDiagnostics(const nlohmann::json& a_snapshot)
	{
		if (!_diagnosticsAvailable) return;
		const auto serialized = Json::Dump(a_snapshot);
		if (serialized == _lastSnapshot) return;
		_lastSnapshot = serialized;
		nlohmann::json keep = nlohmann::json::array();
		for (const auto& id : _directFailures) keep.push_back(id);
		if (const auto* issues = Json::GetArray(a_snapshot, "issues")) for (const auto& issue : *issues) {
			if (Json::Get(issue, "status", "active") != "active") continue;
			const auto id = BoundedId(Json::Get(issue, "id", "runtime")); keep.push_back(id);
			const auto code = Json::Get(issue, "code", "runtime.failure");
			const auto message = Json::Get(issue, "subject", code);
			const auto severity = Json::Get(issue, "severity", "warning") == "error" ?
				OSFSettings::API::Diagnostics::Severity::kError : OSFSettings::API::Diagnostics::Severity::kWarning;
			auto context = issue.contains("context") ? Json::Dump(issue["context"]) : std::string("{}");
			_diagnostics.Report("osfui", id.c_str(), code.c_str(), severity, message.c_str(), context.c_str());
		}
		const auto keepJson = Json::Dump(keep); _diagnostics.ClearExcept("osfui", keepJson.c_str());
	}

	void OSFSettingsClient::AcquireInputSuppression()
	{
		if (!_suppressionLease && _settingsAvailable) _suppressionLease = _settings.AcquireSuppression("osfui-web-focus");
	}

	void OSFSettingsClient::ReleaseInputSuppression()
	{
		if (!_suppressionLease) return;
		_settings.ReleaseSuppression(_suppressionLease); _suppressionLease = 0;
	}
}
