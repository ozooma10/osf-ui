#include "Runtime/Runtime.h"

#include "BrowserMessages.h"
#include "BrowserSurface.h"
#include "Input/MenuEventSink.h"
#include "Input/OverlayInputHook.h"

namespace OSFUI
{
	namespace
	{
		constexpr std::uint32_t kVkEscape{ 0x1B };
		constexpr std::uint32_t kVkF12{ 0x7B };
	}

	bool Runtime::IsInputCaptured() const
	{
		return m_inputCapture.CaptureRequested() && (m_visible.load() || SurfaceService::Get().InputRenderer());
	}

	bool Runtime::OnGameWindowKeyboard(const osfui::browser::msg::Keyboard& a_key)
	{
		if (m_developerMode && IsInputCaptured() && a_key.vk == kVkF12) {
			if (a_key.down && !a_key.repeat) m_devToolsRequested.store(true);
			return true;
		}
		if (IsInputCaptured() && a_key.vk == kVkEscape) {
			if (a_key.down && !a_key.repeat) {
                if (SurfaceService::Get().InputRenderer()) SurfaceService::Get().RequestBack();
                else EnqueuePresentationRequest(ViewPresentationRequest::Back);
            }
			return true;
		}
		if (!IsInputCaptured()) return false;
		if (auto renderer=InputRenderer()) renderer->InjectKeyboard(a_key);
		return true;
	}

	void Runtime::OnGameWindowText(const osfui::browser::msg::TextInput& a_text)
	{
		if (!IsInputCaptured()) return;
		if (auto renderer=InputRenderer()) renderer->InjectText(a_text);
	}

	void Runtime::OnGameWindowActivation(bool a_active)
	{
		if (m_renderer) m_renderer->SetWindowActive(a_active);
		if (auto renderer=SurfaceService::Get().InputRenderer()) renderer->SetWindowActive(a_active);
		if (!a_active) { m_relativePointer.RequestCancel(); SurfaceService::Get().CancelInteraction(); }
	}

	void Runtime::OnGameWindowRecreated()
	{
		if (m_renderer) m_renderer->GameWindowChanged();
	}

	void Runtime::OnGameWindowMouseAbsolute(int a_clientX, int a_clientY, int a_clientW, int a_clientH)
	{
		if (!IsInputCaptured()) return;
		if (SurfaceService::Get().FullscreenActive()) {
			m_surfacePointer.UpdateAbsolute(a_clientX, a_clientY, a_clientW, a_clientH);
			return;
		}
		if (SurfaceService::Get().InputRenderer() || !m_renderer) return;
		m_pointerInput.UpdateAbsolute(a_clientX, a_clientY, a_clientW, a_clientH);
	}

	bool Runtime::OnGameWindowMouseRelative(int a_dx, int a_dy)
	{
		return m_relativePointer.AccumulateMotion(a_dx, a_dy);
	}

	void Runtime::OnGameWindowMouseButton(int a_button, bool a_down)
	{
		if (!IsInputCaptured()) return;
		if (SurfaceService::Get().FullscreenActive()) {
			// Letterbox bars swallow presses; releases always reach the page.
			auto surface = SurfaceService::Get().InputRenderer();
			if (surface && (!a_down || m_surfacePointer.CanSendPointer())) {
				const auto cursor = m_surfacePointer.CursorPosition();
				surface->InjectMouseButton(cursor.x, cursor.y, a_button, a_down);
			}
			return;
		}
		if (SurfaceService::Get().InputRenderer() || !m_renderer) return;
		// Always forward releases, including while geometry is suspended.
		if (!a_down || m_pointerInput.CanSendPointer()) {
			const auto cursor = m_pointerInput.CursorPosition();
			m_renderer->InjectMouseButton(cursor.x, cursor.y, a_button, a_down);
		}
		if (a_button == 0 && !a_down) m_relativePointer.RequestEnd();
	}

	void Runtime::OnGameWindowMouseWheel(int a_wheelDelta)
	{
		if (!IsInputCaptured()) return;
		if (SurfaceService::Get().FullscreenActive()) {
			auto surface = SurfaceService::Get().InputRenderer();
			if (surface && m_surfacePointer.CanSendPointer()) {
				const auto cursor = m_surfacePointer.CursorPosition();
				surface->InjectPhysicalMouseWheel(cursor.x, cursor.y, a_wheelDelta);
			}
			return;
		}
		if (SurfaceService::Get().InputRenderer() || !m_renderer) return;
		if (m_relativePointer.AccumulateWheel(a_wheelDelta)) return;
		if (!m_pointerInput.CanSendPointer()) return;
		const auto cursor = m_pointerInput.CursorPosition();
		m_renderer->InjectPhysicalMouseWheel(cursor.x, cursor.y, a_wheelDelta);
	}

	void Runtime::ReconcileInputSuppression()
	{
		if (m_inputCapture.ReconcileSuppression(m_presentation.DesiredCapture() || static_cast<bool>(SurfaceService::Get().InputRenderer()), m_osfSettings)) return;
		SurfaceService::Get().FinishInteraction("input-unavailable");
		FailPendingOpen("Input capture is unavailable.");
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
		auto active = SurfaceService::Get().InputView();
		if (!active) active = m_presentation.ActiveMenu();
		if (!active) {
			endSession();
			return;
		}

		const auto mode = m_viewInputGrants.GamepadModeFor(*active);
		const auto frame = m_gamepadSession.Update(m_gamepadSource.Poll(m_nowSeconds), mode, m_nowSeconds);

		const auto applyAction = [this](GamepadSession::Action a_action) {
			std::uint32_t key = 0;
			switch (a_action) {
			case GamepadSession::Action::kUp:       key = 0x26; break;  // VK_UP
			case GamepadSession::Action::kDown:     key = 0x28; break;  // VK_DOWN
			case GamepadSession::Action::kLeft:     key = 0x25; break;  // VK_LEFT
			case GamepadSession::Action::kRight:    key = 0x27; break;  // VK_RIGHT
			case GamepadSession::Action::kActivate: key = 0x0D; break;  // VK_RETURN
			case GamepadSession::Action::kBack:
				if (SurfaceService::Get().InputRenderer()) SurfaceService::Get().RequestBack();
                else EnqueuePresentationRequest(ViewPresentationRequest::Back);
				return;
			case GamepadSession::Action::kNone:
				return;
			}
			// Discrete down+up tap: a missed release cannot leave a stuck key.
			if (auto renderer=InputRenderer()) { renderer->InjectKeyEvent(key, true); renderer->InjectKeyEvent(key, false); }
		};

		for (std::size_t i = 0; i < frame.buttonEdgeCount; ++i) {
			const auto& edge = frame.buttonEdges[i];
            if (edge.down && SurfaceService::Get().InputRenderer() &&
                (edge.idCode == XInputButton::kBack ||
                    (edge.idCode == XInputButton::kB && *active == osfui::browser::kBrowserView))) {
                SurfaceService::Get().RequestBack();
                continue;
            }
			if (m_bridge) {
				m_bridge->Emit(*active, "ui.gamepad", nlohmann::json{ { "kind", "button" }, { "button", { { "id", edge.idCode }, { "down", edge.down } } } });
			}
			applyAction(edge.action);
		}

		if (m_bridge && frame.axesChanged) {
			m_bridge->Emit(*active, "ui.gamepad", nlohmann::json{ { "kind", "stick" }, { "axes", { { "lx", frame.axes.lx }, { "ly", frame.axes.ly }, { "rx", frame.axes.rx }, { "ry", frame.axes.ry } } } });
		}

		applyAction(frame.navigationAction);
		if (frame.wheelDelta != 0 && SurfaceService::Get().FullscreenActive()) {
			if (auto surface = SurfaceService::Get().InputRenderer()) {
				const auto cursor = m_surfacePointer.CursorPosition();
				surface->InjectMouseWheel(cursor.x, cursor.y, frame.wheelDelta);
			}
		} else if (frame.wheelDelta != 0 && !SurfaceService::Get().InputRenderer()) {
			const auto cursor = m_pointerInput.CursorPosition();
			m_renderer->InjectMouseWheel(cursor.x, cursor.y, frame.wheelDelta);
		}
	}

}

namespace OSFUI
{
    std::shared_ptr<BrowserHostClient> Runtime::InputRenderer() const
    {
        auto surface=SurfaceService::Get().InputRenderer(); return surface ? surface : m_renderer;
    }
    void Runtime::UpdateSurfaceInteraction()
    {
        if (!SurfaceService::Get().WantsInteraction()) return;
        const auto before=SurfaceService::Get().InputRenderer();
        // Neutral input gates admission only. Keep active-session cancellation
        // checks below without rescanning every virtual key each frame.
        bool neutral=false;
        if (!before) {
            const auto pad=m_gamepadSource.Poll(m_nowSeconds);
            neutral = OverlayInputHook::KeysNeutral() && !pad.buttons && std::abs(pad.lx)<0.15f && std::abs(pad.ly)<0.15f && std::abs(pad.rx)<0.15f && std::abs(pad.ry)<0.15f;
        }
        const bool allowed=m_inputCapture.IntegrationAvailable() && !MenuEventSink::SurfaceBlocked() &&
            !m_presentation.ActiveMenu() && !m_viewOpens.PendingMenu() && OverlayInputHook::IsGameWindowActive();
        SurfaceService::Get().UpdateInteraction(allowed,neutral,static_cast<bool>(m_compositor),[this]{return m_osfSettings.AcquireInputSuppression();});
        if (before != SurfaceService::Get().InputRenderer()) {
            (void)m_gamepadSession.End(); m_gamepadSource.Reset(); m_inputCapture.ResetBrowserFocus();
            if (const auto size=SurfaceService::Get().FullscreenSize()) {
                const ViewSize surface{size->first,size->second};
                m_surfacePointer.PublishGeometry(surface,surface);
            }
            OverlayInputHook::RequestStateRefresh();
        }
    }
    void Runtime::DrainSurfacePointer()
    {
        if (!SurfaceService::Get().FullscreenActive()) { m_surfacePointer.DiscardMouseMove(); return; }
        const auto move=m_surfacePointer.TakeMouseMove();
        if (auto surface=SurfaceService::Get().InputRenderer(); move && surface) surface->InjectMouseMove(move->x,move->y);
    }
}
