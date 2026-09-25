#include "Runtime/Runtime.h"

#include "Wv2Messages.h"

namespace OSFUI
{
	namespace
	{
		constexpr std::uint32_t kVkEscape{ 0x1B };
		constexpr std::uint32_t kVkF12{ 0x7B };
	}

	bool Runtime::IsInputCaptured() const
	{
		return _initialized && _inputCapture.CaptureRequested() && m_visible.load();
	}

	bool Runtime::OnGameWindowKeyboard(const osfui::wv2::msg::Keyboard& a_key)
	{
		if (_developerMode && IsInputCaptured() && a_key.vk == kVkF12) {
			if (a_key.down && !a_key.repeat) _devToolsRequested.store(true);
			return true;
		}
		if (IsInputCaptured() && a_key.vk == kVkEscape) {
			if (a_key.down && !a_key.repeat) EnqueuePresentationRequest(ViewPresentationRequest::Back);
			return true;
		}
		if (!IsInputCaptured()) return false;
		if (_renderer) _renderer->InjectKeyboard(a_key);
		return true;
	}

	void Runtime::OnGameWindowText(const osfui::wv2::msg::TextInput& a_text)
	{
		if (!IsInputCaptured()) return;
		if (_renderer) _renderer->InjectText(a_text);
	}

	void Runtime::OnGameWindowActivation(bool a_active)
	{
		if (_renderer) _renderer->SetWindowActive(a_active);
		if (!a_active) _relativePointer.RequestCancel();
	}

	void Runtime::OnGameWindowMouseAbsolute(int a_clientX, int a_clientY, int a_clientW, int a_clientH)
	{
		if (!IsInputCaptured() || !_renderer) return;
		_pointerInput.UpdateAbsolute(a_clientX, a_clientY, a_clientW, a_clientH);
	}

	bool Runtime::OnGameWindowMouseRelative(int a_dx, int a_dy)
	{
		return _relativePointer.AccumulateMotion(a_dx, a_dy);
	}

	void Runtime::OnGameWindowMouseButton(int a_button, bool a_down)
	{
		if (!IsInputCaptured() || !_renderer) return;
		// Always forward releases, including while geometry is suspended.
		if (!a_down || _pointerInput.CanSendPointer()) {
			const auto cursor = _pointerInput.CursorPosition();
			_renderer->InjectMouseButton(cursor.x, cursor.y, a_button, a_down);
		}
		if (a_button == 0 && !a_down) _relativePointer.RequestEnd();
	}

	void Runtime::OnGameWindowMouseWheel(int a_wheelDelta)
	{
		if (!IsInputCaptured() || !_renderer) return;
		if (_relativePointer.AccumulateWheel(a_wheelDelta)) return;
		if (!_pointerInput.CanSendPointer()) return;
		const auto cursor = _pointerInput.CursorPosition();
		_renderer->InjectPhysicalMouseWheel(cursor.x, cursor.y, a_wheelDelta);
	}

	bool Runtime::ReconcileInputSuppression()
	{
		if (_inputCapture.ReconcileSuppression(_presentation.DesiredCapture(), _osfSettings)) return true;
		_viewOpens.SuspendMenus();
		_presentation.CloseActiveMenu();
		return false;
	}

	void Runtime::ReconcileFocusMenu()
	{
		if (!ReconcileInputSuppression()) ApplyViewPresentationPolicy();
		_inputCapture.ReconcileFocusMenu(_presentation.DesiredCapture(), _nowSeconds);
	}

	void Runtime::RouteGamepadInput()
	{
		const auto endSession = [this] {
			if (m_gamepadSession.End()) {
				m_gamepadSource.Reset();
			}
		};

		if (!IsInputCaptured() || !_renderer) {
			endSession();
			return;
		}
		const auto active = _presentation.ActiveMenu();
		if (!active) {
			endSession();
			return;
		}

		const auto mode = m_viewInputGrants.GamepadModeFor(*active);
		const auto frame = m_gamepadSession.Update(m_gamepadSource.Poll(), mode, _nowSeconds);

		const auto applyAction = [this](GamepadSession::Action a_action) {
			std::uint32_t key = 0;
			switch (a_action) {
			case GamepadSession::Action::kUp:       key = 0x26; break;  // VK_UP
			case GamepadSession::Action::kDown:     key = 0x28; break;  // VK_DOWN
			case GamepadSession::Action::kLeft:     key = 0x25; break;  // VK_LEFT
			case GamepadSession::Action::kRight:    key = 0x27; break;  // VK_RIGHT
			case GamepadSession::Action::kActivate: key = 0x0D; break;  // VK_RETURN
			case GamepadSession::Action::kBack:
				EnqueuePresentationRequest(ViewPresentationRequest::Back);
				return;
			case GamepadSession::Action::kNone:
				return;
			}
			// Discrete down+up tap: a missed release cannot leave a stuck key.
			_renderer->InjectKeyEvent(key, true);
			_renderer->InjectKeyEvent(key, false);
		};

		for (std::size_t i = 0; i < frame.buttonEdgeCount; ++i) {
			const auto& edge = frame.buttonEdges[i];
			if (_bridge) {
				_bridge->Emit(*active, "ui.gamepad", nlohmann::json{ { "kind", "button" }, { "button", { { "id", edge.idCode }, { "down", edge.down } } } });
			}
			applyAction(edge.action);
		}

		if (_bridge && frame.axesChanged) {
			_bridge->Emit(*active, "ui.gamepad", nlohmann::json{ { "kind", "stick" }, { "axes", { { "lx", frame.axes.lx }, { "ly", frame.axes.ly }, { "rx", frame.axes.rx }, { "ry", frame.axes.ry } } } });
		}

		applyAction(frame.navigationAction);
		if (frame.wheelDelta != 0) {
			const auto cursor = _pointerInput.CursorPosition();
			_renderer->InjectMouseWheel(cursor.x, cursor.y, frame.wheelDelta);
		}
	}

}
