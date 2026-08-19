#include "Runtime/Runtime.h"

#include <algorithm>
#include <optional>

#include "API/PapyrusApi.h"
#include "Bindings/InputModes.h"
#include "Core/Log.h"
#include "Input/ControlLayer.h"
#include "Input/FocusMenu.h"
#include "Input/KeyNames.h"
#include "Input/MenuMode.h"
#include "Input/SimPause.h"

namespace OSFUI
{
	void Runtime::NotifyGameWindowFocused()
	{
		if (IsInputCaptured()) {
			_nativeFocusRefreshRequested.store(true, std::memory_order_release);
		}
	}

    namespace
    {
        constexpr KeyCode kVkF12 { 0x7B };
        constexpr ScanCode kUnnameableScan{ 0xFFFF };
        constexpr ScanCode kScanEscape{ 0x01 };
        constexpr ScanCode kScanLWin{ 0xDB };
        constexpr ScanCode kScanRWin{ 0xDC };
    }

    
	bool Runtime::IsInputCaptured() const
	{
		return _initialized && _captureInput.load() && m_visible.load();
	}

	bool Runtime::OnGameWindowKey(std::uint32_t a_vkCode, ScanCode a_scanCode, bool a_down)
	{
		if (_captureArmed.load()) {
            // PrintScreen doesnt deliver keydown, so when armed release counts as press
			constexpr ScanCode kScanPrintScreen = 0xB7;
			if (a_down || a_scanCode == kScanPrintScreen) {
				_capturedScan.store(a_scanCode != kInvalidScanCode ? a_scanCode : kUnnameableScan);
				_captureArmed.store(false);
				_captureUpScan = a_scanCode;
			}
			return true;
		}
		const auto captureUpScan = _captureUpScan.load();
		if (captureUpScan != kInvalidScanCode && a_scanCode == captureUpScan && !a_down) {
			_captureUpScan = kInvalidScanCode;
			return true;
		}

		if (_developerMode && a_vkCode == kVkF12) {
			if (a_down) {
				_devToolsRequested.store(true);
			}
			return true;
		}

		if (a_down) {
			_hotkeys.OnKeyDown(a_scanCode);
		}

		const auto toggleKey = _toggleKey.load(std::memory_order_acquire);
		const bool captured = IsInputCaptured();
		const bool isToggle = toggleKey != kInvalidScanCode && a_scanCode == toggleKey;
		if (a_down) {
			if (isToggle) {
				const auto requestedAt = std::chrono::duration_cast<std::chrono::nanoseconds>(ColdOpenClock::now().time_since_epoch()).count();
				_lastToggleRequestNanos.store(requestedAt, std::memory_order_release);
				EnqueuePresentationRequest(ViewPresentationRequest::ToggleDefault);
			} else if (captured && a_scanCode == kScanEscape) {
				EnqueuePresentationRequest(ViewPresentationRequest::Back);
			} else if (captured && _renderer) {
				_renderer->InjectKeyEvent(a_vkCode, true);
			} else if (Log::DebugEnabled()) {
				REX::DEBUG("Runtime: OnGameWindowKey down (vk {}, scan {}) passed to the game", a_vkCode, a_scanCode);
			}
		} else {
			if (captured && _renderer) {
				_renderer->InjectKeyEvent(a_vkCode, false);
			} else if (Log::DebugEnabled()) {
				REX::DEBUG("Runtime: OnGameWindowKey up (vk {}, scan {}) passed to the game", a_vkCode, a_scanCode);
			}
		}
		return captured || isToggle;
	}

	bool Runtime::OnNativeAcceleratorKey(std::uint32_t a_vkCode, std::uint32_t a_scanCode, bool a_down)
	{
		const auto scan = static_cast<ScanCode>(a_scanCode);
		const auto toggleKey = _toggleKey.load(std::memory_order_acquire);
		const bool frameworkOwned = _captureArmed.load() || (_captureUpScan.load() != kInvalidScanCode && scan == _captureUpScan.load()) ||
			(toggleKey != kInvalidScanCode && scan == toggleKey) || (_developerMode && a_vkCode == kVkF12) || (a_vkCode == 0x1B && IsInputCaptured());
		return frameworkOwned && OnGameWindowKey(a_vkCode, scan, a_down);
	}

	void Runtime::OnGameWindowMouseAbsolute(int a_clientX, int a_clientY, int a_clientW, int a_clientH)
	{
		if (!IsInputCaptured() || !_renderer || a_clientW <= 0 || a_clientH <= 0) {
			return;
		}

        //Need to scale from window -> view (height capped)
		const auto viewW = static_cast<float>(_viewWidth.load(std::memory_order_relaxed));
		const auto viewH = static_cast<float>(_viewHeight.load(std::memory_order_relaxed));
		_cursorX.store(std::clamp(static_cast<float>(a_clientX) * viewW / static_cast<float>(a_clientW), 0.0f, viewW - 1.0f), std::memory_order_relaxed);
		_cursorY.store(std::clamp(static_cast<float>(a_clientY) * viewH / static_cast<float>(a_clientH), 0.0f, viewH - 1.0f), std::memory_order_relaxed);
		QueueMouseMove();
	}

	void Runtime::QueueMouseMove()
	{
		const auto x = static_cast<std::uint32_t>(static_cast<int>(_cursorX.load(std::memory_order_relaxed)));
		const auto y = static_cast<std::uint32_t>(static_cast<int>(_cursorY.load(std::memory_order_relaxed)));
		_pendingMouseMove.store((static_cast<std::uint64_t>(x) << 32) | y);
	}

	void Runtime::OnGameWindowMouseButton(int a_button, bool a_down)
	{
		if (!IsInputCaptured() || !_renderer) {
			return;
		}
		_renderer->InjectMouseButton(static_cast<int>(_cursorX.load(std::memory_order_relaxed)), static_cast<int>(_cursorY.load(std::memory_order_relaxed)), a_button, a_down);
	}

	void Runtime::OnGameWindowMouseWheel(int a_wheelDelta)
	{
		if (!IsInputCaptured() || !_renderer) {
			return;
		}
		_renderer->InjectPhysicalMouseWheel(static_cast<int>(_cursorX.load(std::memory_order_relaxed)), static_cast<int>(_cursorY.load(std::memory_order_relaxed)), a_wheelDelta);
	}

	void Runtime::ReconcileFocusMenu()
	{
		const bool wantOpen = _presentation.DesiredCapture();
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
			_focusMenuMismatchSince = _uptime;
			return;
		}
		if (_uptime - _focusMenuMismatchSince < kHealSeconds) {
			return;
		}
		REX::WARN("FocusMenu: engine state diverged from requested (want {}, engine {}) for {:.1f}s; re-sending {} (watchdog)", wantOpen ? "open" : "closed", wantOpen ? "closed" : "open", _uptime - _focusMenuMismatchSince, wantOpen ? "kShow" : "kHide");
		_focusMenuMismatchSince = -1.0;  // re-arm: another full window before the next retry
		if (wantOpen) {
			FocusMenu::Open();
		} else {
			FocusMenu::Close();
		}
	}

	void Runtime::ReconcileSimPause()
	{
		SimPause::Apply(_presentation.DesiredPause());
	}

	void Runtime::RouteGamepadInput(double a_deltaSeconds)
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
		const auto frame = m_gamepadSession.Update(m_gamepadSource.Poll(), mode, a_deltaSeconds, _uptime);

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
			_renderer->InjectMouseWheel(static_cast<int>(_cursorX.load(std::memory_order_relaxed)), static_cast<int>(_cursorY.load(std::memory_order_relaxed)), frame.wheelDelta);
		}
	}

	void Runtime::ReconcileControlLayer()
	{
		ControlLayer::Apply(_presentation.DesiredCapture());
		FocusMenu::SetGamepadCapture(IsInputCaptured());
	}

	void Runtime::DrainKeyCapture()
	{
		const ScanCode scan = _capturedScan.exchange(kInvalidScanCode);
		if (scan == kInvalidScanCode) {
			return;  // nothing captured this tick
		}
		if (!_bridge || _captureView.empty()) {
			return;  // nobody to answer
		}
		const bool reserved = scan == kScanLWin || scan == kScanRWin;
		const std::string name = (scan == kScanEscape || reserved) ? std::string{} : KeyName(scan);
		const bool cancelled = name.empty();
		nlohmann::json payload{
			{ "mod", _captureMod },
			{ "key", _captureKey },
			{ "name", name },
			{ "cancelled", cancelled },
		};
		if (cancelled) {
			payload["reason"] = (scan == kScanEscape) ? "escape" : reserved ? "reserved" : "unnameable";
		} else {
			payload["label"] = KeyLabelFor(name);
		}
		if (!cancelled && _settings) {
			if (auto conflicts = _settings->Store().ConflictsFor(scan, _captureMod, _captureKey); !conflicts.empty()) {
				payload["conflicts"] = std::move(conflicts);
			}
		}
		_bridge->Emit(_captureView, "settings.captured", payload);
		REX::DEBUG("Runtime: key capture -> {} (scan {:#04x}) ({}.{})", cancelled ? "(cancelled)" : name, scan, _captureMod, _captureKey);
		_captureView.clear();
		_captureMod.clear();
		_captureKey.clear();
		_captureUpScan = kInvalidScanCode;
	}

	void Runtime::CancelArmedKeyCapture()
	{
		if (!_captureArmed.exchange(false)) {
			return;
		}
		_captureUpScan = kInvalidScanCode;
		if (_bridge && !_captureView.empty()) {
			nlohmann::json payload{
				{ "mod", _captureMod },
				{ "key", _captureKey },
				{ "name", "" },
				{ "cancelled", true },
			};
			_bridge->Emit(_captureView, "settings.captured", payload);
		}
		REX::DEBUG("Runtime: armed key capture cancelled by menu close ({}.{})", _captureMod, _captureKey);
		_captureView.clear();
		_captureMod.clear();
		_captureKey.clear();
	}

	void Runtime::DrainHotkeys()
	{
		std::optional<bool> inGameMenu;
		_hotkeys.Drain([this, &inGameMenu](const std::string& a_mod, const std::string& a_key) {
			if (!inGameMenu) {
				inGameMenu = MenuMode::AnyGameMenuOpen();
			}
			if (*inGameMenu) {
				REX::DEBUG("Runtime: hotkey {}.{} dropped (game menu open)", a_mod, a_key);
				return;
			}
			if (_settings) {
				const auto scope = _settings->Store().ScopeForHotkey(a_mod, a_key);
				if (scope.scoped) {
					const auto mode = _controlMap.CurrentMode();
					if (!_controlMap.Available() || !mode || !ModesOverlap(scope.modes, ModeBit(*mode))) {
						REX::DEBUG("Runtime: hotkey {}.{} dropped (scoped modes {:#x}, current mode {})", a_mod, a_key, scope.modes, mode ? GameplayModeName(*mode) : "unavailable");
						return;
					}
				}
			}

			API::BridgeApi::Get().Hotkeys().OnFired(a_mod, a_key);
			if (_settings) {
				_settings->PushHotkey(a_mod, a_key);
			}

			API::Papyrus::OnHotkey(a_mod, a_key);

			if (_settings) {
				if (const auto target = _settings->Store().GetHotkeyTarget(a_mod, a_key)) {
					const auto result = API::Papyrus::DispatchStaticHotkey(target->script, target->function, a_mod, a_key);
					if (result == API::Papyrus::StaticDispatchResult::kQueued) {
						_runtimeHealth.ResolveHotkeyTarget(a_mod, a_key);
					} else {
						const auto reason = result == API::Papyrus::StaticDispatchResult::kVmUnavailable ?
							"the Papyrus VM is unavailable" :
							"Papyrus rejected the call; the script may be missing, the function may be absent or non-GLOBAL, or its signature may not be (string, string)";
						_runtimeHealth.ReportHotkeyTargetFailure(a_mod, a_key, target->script, target->function, reason);
					}
				} else {
					_runtimeHealth.ResolveHotkeyTarget(a_mod, a_key);
				}
			}
			REX::DEBUG("Runtime: hotkey fired for {}.{}", a_mod, a_key);
		});
	}

}
