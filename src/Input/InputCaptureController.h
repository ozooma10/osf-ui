#pragma once

#include <atomic>

namespace OSFUI
{
	class OSFSettingsClient;
	class WebView2HostWebRenderer;

	// Main-thread engine integration and focus bookkeeping. Only CaptureRequested
	// is queried by WndProc; Runtime retains presentation and failure policy.
	class InputCaptureController
	{
	public:
		void ObserveLifecycle(bool a_postDataLoadedReady);
		bool EnsureIntegration(bool a_postDataLoadedReady);
		bool MenuEventsAvailable() const { return _menuEventsAvailable; }
		bool IntegrationAttempted() const { return _integrationAttempted; }
		bool IntegrationAvailable() const { return _integrationAvailable; }

		bool ReconcileSuppression(bool a_wantsCapture, OSFSettingsClient& a_settings);
		void ReconcileFocusMenu(bool a_wantsCapture, double a_now);
		void PublishCapture(bool a_wantsCapture);
		bool CaptureRequested() const;
		void ReconcileBrowserFocus(WebView2HostWebRenderer* a_renderer, bool a_visible, bool a_hasActiveMenu);
		void ResetBrowserFocus();
		void ReconcileControlLayer(bool a_wantsCapture, bool a_inputCaptured);

	private:
		bool _menuEventsAttempted{ false };
		bool _menuEventsAvailable{ false };
		bool _integrationAttempted{ false };
		bool _integrationAvailable{ false };
		bool _focusMenuOpen{ false };
		double _focusMenuMismatchSince{ -1.0 };
		bool _browserFocusGranted{ false };
		std::atomic_bool _captureRequested{ false };  // main -> WndProc
	};
}
