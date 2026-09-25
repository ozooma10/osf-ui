#include "Input/InputCaptureController.h"

#include "Core/Log.h"
#include "Dependency/OSFSettingsClient.h"
#include "Input/ControlLayer.h"
#include "Input/FocusMenu.h"
#include "Input/MenuEventSink.h"
#include "Input/OverlayInputHook.h"
#include "Input/UiLayoutGuard.h"
#include "Render/WebView2HostWebRenderer.h"

namespace OSFUI
{
	bool InputCaptureController::EnsureIntegration(bool a_postDataLoadedReady)
	{
		if (_integrationAttempted) return _integrationAvailable;
		if (!a_postDataLoadedReady) return false;
		_integrationAttempted = true;
		if (!UiLayoutGuard::VerifyUiLayout()) {
			REX::ERROR("Runtime: UI layout guard failed; skipping ALL UI integration (menu events, FocusMenu and the WndProc hook stay uninstalled; capturing menus are unavailable)");
			return false;
		}
		const bool menuEventsInstalled = _menuEventsAvailable;
		const bool focusMenuRegistered = FocusMenu::Register();
		const bool inputInstalled = OverlayInputHook::Install();
		_integrationAvailable = menuEventsInstalled && focusMenuRegistered && inputInstalled;
		if (!_integrationAvailable) {
			REX::ERROR("Runtime: required input integration is unavailable; menus that capture input will be refused this session");
			return false;
		}
		REX::INFO("Runtime: lazy web-input hook installed above OSF Settings input handling");
		return true;
	}

	bool InputCaptureController::ReconcileSuppression(bool a_wantsCapture, OSFSettingsClient& a_settings)
	{
		if (!a_wantsCapture) {
			// Retry failed releases after the close edge, including cancellation and renderer failure.
			a_settings.ReleaseInputSuppression();
			return true;
		}
		if (a_settings.AcquireInputSuppression()) return true;
		a_settings.ReportFailure("input.hotkey-block", "input.hotkey-block",
			"The WebView cannot capture input because OSF hotkeys could not be blocked");
		return false;
	}

	void InputCaptureController::ReconcileFocusMenu(bool a_wantsCapture, double a_now)
	{
		const bool wantOpen = a_wantsCapture;
		if (wantOpen != _focusMenuOpen) {
			_focusMenuOpen = wantOpen;
			_focusMenuMismatchSince = -1.0;  // fresh request: full grace window
			if (wantOpen) {
				FocusMenu::Open();
			} else {
				FocusMenu::Close();
			}
			return;
		}

		if (!FocusMenu::IsRegistered()) {
			return;
		}
		const bool engineOpen = FocusMenu::IsOpenInEngine();
		if (engineOpen == wantOpen) {
			_focusMenuMismatchSince = -1.0;
			return;
		}
		constexpr double kHealSeconds = 1.0;
		if (_focusMenuMismatchSince < 0.0) {
			_focusMenuMismatchSince = a_now;
			return;
		}
		if (a_now - _focusMenuMismatchSince < kHealSeconds) {
			return;
		}
		REX::WARN("FocusMenu: engine state diverged from requested (want {}, engine {}) for {:.1f}s; re-sending {} (watchdog)", wantOpen ? "open" : "closed", wantOpen ? "closed" : "open", a_now - _focusMenuMismatchSince, wantOpen ? "kShow" : "kHide");
		_focusMenuMismatchSince = -1.0;  // re-arm: another full window before the next retry
		if (wantOpen) {
			FocusMenu::Open();
		} else {
			FocusMenu::Close();
		}
	}

	void InputCaptureController::ReconcileBrowserFocus(
		WebView2HostWebRenderer* a_renderer, bool a_visible, bool a_hasActiveMenu)
	{
		if (!a_renderer) {
			return;
		}
		const bool wantsCapture = a_visible && CaptureRequested() && a_hasActiveMenu;
		// Grant browser input only after the menu stack admits the input-owning
		// sentinel. In forwarded mode this grant does not transfer OS focus.
		const bool focusMenuReady = !wantsCapture || (FocusMenu::IsRegistered() && FocusMenu::IsOpenInEngine());
		const bool want = wantsCapture && focusMenuReady;
		if (want == _browserFocusGranted) {
			return;
		}
		_browserFocusGranted = want;
		a_renderer->SetInputFocus(want);
	}

	void InputCaptureController::ObserveLifecycle(bool a_postDataLoadedReady)
	{
		if (a_postDataLoadedReady && !_menuEventsAttempted) {
			_menuEventsAttempted = true;
			_menuEventsAvailable = UiLayoutGuard::VerifyUiLayout() && MenuEventSink::Install();
		}
	}

	void InputCaptureController::PublishCapture(bool a_wantsCapture)
	{
		if (_captureRequested.exchange(a_wantsCapture) != a_wantsCapture) {
			OverlayInputHook::RequestStateRefresh();
		}
	}

	bool InputCaptureController::CaptureRequested() const
	{
		return _captureRequested.load();
	}

	void InputCaptureController::ResetBrowserFocus()
	{
		_browserFocusGranted = false;
	}

	void InputCaptureController::ReconcileControlLayer(bool a_wantsCapture, bool a_inputCaptured)
	{
		ControlLayer::Apply(a_wantsCapture);
		FocusMenu::SetGamepadCapture(a_inputCaptured);
	}
}
