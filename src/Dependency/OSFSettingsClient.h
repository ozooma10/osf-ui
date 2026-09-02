#pragma once

#include <nlohmann/json.hpp>

#include "OSFSettings.h"
#ifdef ERROR
#undef ERROR
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace OSFUI
{
	class OSFSettingsClient final
	{
	public:
		bool Initialize();
		[[nodiscard]] bool Available() const { return _settingsAvailable && _diagnosticsAvailable; }
		[[nodiscard]] bool DeveloperMode() const { return _developerMode; }
		[[nodiscard]] bool HighRefreshCapture() const { return _highRefreshCapture; }
		void SyncDiagnostics(const nlohmann::json& a_snapshot);
		void ReportFailure(std::string_view a_id, std::string_view a_code, std::string_view a_message,
			const nlohmann::json& a_context = nlohmann::json::object());
		void ClearFailure(std::string_view a_id);
		void AcquireInputSuppression();
		void ReleaseInputSuppression();

	private:
		[[nodiscard]] static std::string BoundedId(std::string_view a_id);
		OSFSettings::API::Settings::Client _settings;
		OSFSettings::API::Diagnostics::Client _diagnostics;
		bool _settingsAvailable{};
		bool _diagnosticsAvailable{};
		bool _developerMode{};
		bool _highRefreshCapture{};
		std::uint64_t _suppressionLease{};
		std::string _lastSnapshot;
		std::unordered_set<std::string> _directFailures;
	};
}
