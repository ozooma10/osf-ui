#pragma once

#include <atomic>

namespace OSFUI
{
	class OSFSettingsClient;
	class WebView2HostWebRenderer;

	// Runtime engine integration and focus bookkeeping. Only CaptureRequested
	// is queried by WndProc; Runtime retains presentation and failure policy.
	class InputCaptureController
	{
	public:
		// First runtime tick after kPostPostDataLoad, before processing view requests.
		bool Initialize();
		bool MenuEventsAvailable() const { return m_menuEventsAvailable; }
		bool IntegrationAttempted() const { return m_integrationState != IntegrationState::NotAttempted; }
		bool IntegrationAvailable() const { return m_integrationState == IntegrationState::Available; }

		bool ReconcileSuppression(bool a_wantsCapture, OSFSettingsClient& a_settings);
		void ReconcileFocusMenu(bool a_wantsCapture, double a_now);
		void PublishCapture(bool a_wantsCapture);
		bool CaptureRequested() const;
		void ReconcileBrowserFocus(WebView2HostWebRenderer* a_renderer, bool a_visible, bool a_hasActiveMenu);
		void ResetBrowserFocus();
		void ReconcileControlLayer(bool a_wantsCapture, bool a_inputCaptured);

	private:
		enum class IntegrationState
		{
			NotAttempted,
			Available,
			Failed,
		};

		IntegrationState m_integrationState{ IntegrationState::NotAttempted };
		bool m_menuEventsAvailable{ false };
		bool m_focusMenuOpen{ false };
		double m_focusMenuMismatchSince{ -1.0 };
		bool m_browserFocusGranted{ false };
		std::atomic_bool m_captureRequested{ false };  // runtime -> WndProc
	};
}
