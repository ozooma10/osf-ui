#pragma once

#include "Render/WebView2HostWebRenderer.h"
#include "API/BridgeApi.h"
#include "Diagnostics/HealthReconciler.h"

#include <unordered_map>

namespace OSFUI
{
	class Runtime;

	class RuntimeHealthCoordinator final
	{
	public:
		explicit RuntimeHealthCoordinator(Runtime& a_runtime) : _runtime(a_runtime) {}

		void Pump();
		void OnRendererHealth(const WebView2HostWebRenderer::HealthEvent& a_event);
		void ReportViewLoad(std::string_view a_viewId, bool a_failed, std::string_view a_description, int a_errorCode, std::uint32_t a_attemptsLeft);
		void ReportHotkeyTargetFailure(std::string_view a_mod, std::string_view a_key, std::string_view a_script, std::string_view a_function, std::string_view a_message);
		void ResolveHotkeyTarget(std::string_view a_mod, std::string_view a_key);

	private:
		void DrainPluginHealthReports();
		void SyncSettings();
		void SyncCompatibility();
		void UpdateSystemInfo();
		static std::string HotkeyTargetId(std::string_view a_mod, std::string_view a_key);

		struct HotkeyTargetFailure
		{
			std::string mod;
			std::string key;
			std::string script;
			std::string function;
			std::string message;
		};

		Runtime&         _runtime;
		std::uint64_t    _settingsGeneration{ 0 };
		bool             _settingsSynced{ false };
		HealthReconciler _healthReconciler;

		std::vector<API::BridgeApi::UnsupportedCaller> _unsupportedApiCallers;
		std::unordered_set<std::string> _loggedCompatibility;
		std::unordered_map<std::string, HotkeyTargetFailure> _hotkeyTargetFailures;
		double _nextPoll{ 0.0 };
	};
}
