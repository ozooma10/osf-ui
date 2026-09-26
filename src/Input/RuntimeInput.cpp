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
		return m_initialized && m_inputCapture.CaptureRequested() && m_visible.load();
	}

	bool Runtime::OnGameWindowKeyboard(const osfui::wv2::msg::Keyboard& a_key)
	{
		if (m_developerMode && IsInputCaptured() && a_key.vk == kVkF12) {
			if (a_key.down && !a_key.repeat) m_devToolsRequested.store(true);
			return true;
		}
		if (IsInputCaptured() && a_key.vk == kVkEscape) {
			if (a_key.down && !a_key.repeat) EnqueuePresentationRequest(ViewPresentationRequest::Back);
			return true;
		}
		if (!IsInputCaptured()) return false;
		if (m_renderer) m_renderer->InjectKeyboard(a_key);
		return true;
	}

	void Runtime::OnGameWindowText(const osfui::wv2::msg::TextInput& a_text)
	{
		if (!IsInputCaptured()) return;
		if (m_renderer) m_renderer->InjectText(a_text);
	}

	void Runtime::OnGameWindowActivation(bool a_active)
	{
		if (m_renderer) m_renderer->SetWindowActive(a_active);
		if (!a_active) m_relativePointer.RequestCancel();
	}

	void Runtime::OnGameWindowMouseAbsolute(int a_clientX, int a_clientY, int a_clientW, int a_clientH)
	{
		if (!IsInputCaptured() || !m_renderer) return;
		m_pointerInput.UpdateAbsolute(a_clientX, a_clientY, a_clientW, a_clientH);
	}

	bool Runtime::OnGameWindowMouseRelative(int a_dx, int a_dy)
	{
		return m_relativePointer.AccumulateMotion(a_dx, a_dy);
	}

	void Runtime::OnGameWindowMouseButton(int a_button, bool a_down)
	{
		if (!IsInputCaptured() || !m_renderer) return;
		// Always forward releases, including while geometry is suspended.
		if (!a_down || m_pointerInput.CanSendPointer()) {
			const auto cursor = m_pointerInput.CursorPosition();
			m_renderer->InjectMouseButton(cursor.x, cursor.y, a_button, a_down);
		}
		if (a_button == 0 && !a_down) m_relativePointer.RequestEnd();
	}

	void Runtime::OnGameWindowMouseWheel(int a_wheelDelta)
	{
		if (!IsInputCaptured() || !m_renderer) return;
		if (m_relativePointer.AccumulateWheel(a_wheelDelta)) return;
		if (!m_pointerInput.CanSendPointer()) return;
		const auto cursor = m_pointerInput.CursorPosition();
		m_renderer->InjectPhysicalMouseWheel(cursor.x, cursor.y, a_wheelDelta);
	}

	void Runtime::ReconcileInputSuppression()
	{
		if (m_inputCapture.ReconcileSuppression(m_presentation.DesiredCapture(), m_osfSettings)) return;
		m_viewOpens.CancelMenu();
		m_presentation.CloseActiveMenu();
	}

	void Runtime::RouteGamepadInput()
	{
		const auto endSession = [this] {
			if (m_gamepadSession.End()) {
				m_gamepadSource.Reset();
			}
		};

		if (!IsInputCaptured() || !m_renderer) {
			endSession();
			return;
		}
		const auto active = m_presentation.ActiveMenu();
		if (!active) {
			endSession();
			return;
		}

		const auto mode = m_viewInputGrants.GamepadModeFor(*active);
		const auto frame = m_gamepadSession.Update(m_gamepadSource.Poll(), mode, m_nowSeconds);

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
			m_renderer->InjectKeyEvent(key, true);
			m_renderer->InjectKeyEvent(key, false);
		};

		for (std::size_t i = 0; i < frame.buttonEdgeCount; ++i) {
			const auto& edge = frame.buttonEdges[i];
			if (m_bridge) {
				m_bridge->Emit(*active, "ui.gamepad", nlohmann::json{ { "kind", "button" }, { "button", { { "id", edge.idCode }, { "down", edge.down } } } });
			}
			applyAction(edge.action);
		}

		if (m_bridge && frame.axesChanged) {
			m_bridge->Emit(*active, "ui.gamepad", nlohmann::json{ { "kind", "stick" }, { "axes", { { "lx", frame.axes.lx }, { "ly", frame.axes.ly }, { "rx", frame.axes.rx }, { "ry", frame.axes.ry } } } });
		}

		applyAction(frame.navigationAction);
		if (frame.wheelDelta != 0) {
			const auto cursor = m_pointerInput.CursorPosition();
			m_renderer->InjectMouseWheel(cursor.x, cursor.y, frame.wheelDelta);
		}
	}

}
