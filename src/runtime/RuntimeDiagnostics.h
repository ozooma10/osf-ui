#pragma once

#include "render/IWebRenderer.h"

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
#if defined(OSFUI_WITH_WORLD_SURFACES)
		void OnWorldSurfaceHealth(std::size_t a_index, const IWebRenderer::HealthEvent& a_event);
#endif
		void ReportViewLoad(std::string_view a_viewId, bool a_failed,
			std::string_view a_description, int a_errorCode, std::uint32_t a_attemptsLeft);

	private:
		void DrainPluginReports();
		void SyncSettings();
		void SyncCompatibility();
		void UpdateSystemInfo();

		Runtime&      _runtime;
		std::uint64_t _settingsGeneration{ 0 };
		bool          _settingsSynced{ false };
		std::string   _compatSignature;
		double        _nextPoll{ 0.0 };
	};
}