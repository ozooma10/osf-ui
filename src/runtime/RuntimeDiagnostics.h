#pragma once

#include "render/IWebRenderer.h"
#include "api/BridgeApi.h"
#include "runtime/DiagnosticsReconciler.h"

#include <unordered_map>

namespace OSFUI
{
	class Runtime;

	// Owns System Health reconciliation and its slow-changing signatures. Runtime
	// forwards only the tick, renderer-health, and view-load edges.
	class RuntimeDiagnostics final
	{
	public:
		explicit RuntimeDiagnostics(Runtime& a_runtime) : _runtime(a_runtime) {}

		void Pump();
		void OnRendererHealth(const IWebRenderer::HealthEvent& a_event);
		void ReportViewLoad(std::string_view a_viewId, bool a_failed,
			std::string_view a_description, int a_errorCode, std::uint32_t a_attemptsLeft);
		void ReportHotkeyTargetFailure(std::string_view a_mod, std::string_view a_key,
			std::string_view a_script, std::string_view a_function, std::string_view a_message);
		void ResolveHotkeyTarget(std::string_view a_mod, std::string_view a_key);

	private:
		void DrainPluginReports();
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

		Runtime&      _runtime;
		std::uint64_t _settingsGeneration{ 0 };
		bool          _settingsSynced{ false };
		DiagnosticsReconciler _reconciler;
		// ABI compatibility callers, accumulated once at load and kept for the
		// session: supported 1.x and refused unrelated majors are distinct cards.
		std::vector<API::BridgeApi::LegacyCaller> _legacyApiCallers;
		std::unordered_set<std::string> _loggedCompatibility;
		std::unordered_set<std::string> _legacyPapyrusCallers;
		std::unordered_map<std::string, HotkeyTargetFailure> _hotkeyTargetFailures;
		double        _nextPoll{ 0.0 };
	};
}
