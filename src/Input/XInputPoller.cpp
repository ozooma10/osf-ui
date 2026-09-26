#include "Input/XInputPoller.h"

#include <Windows.h>
#include <Xinput.h>

#include <algorithm>
#include <cstdlib>

namespace OSFUI
{
	namespace
	{
		float NormalizeThumb(SHORT a_value)
		{
			// The negative endpoint has one extra representable value. Clamping keeps both sides in the bridge's documented -1..1 range.
			return std::clamp(static_cast<float>(a_value) / 32767.0f, -1.0f, 1.0f);
		}

		bool ShowsActivity(const XINPUT_STATE& a_state)
		{
			const auto& pad = a_state.Gamepad;
			return pad.wButtons != 0 || pad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD || pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ||
				std::abs(static_cast<int>(pad.sThumbLX)) > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE || std::abs(static_cast<int>(pad.sThumbLY)) > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE ||
				std::abs(static_cast<int>(pad.sThumbRX)) > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE || std::abs(static_cast<int>(pad.sThumbRY)) > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
		}

		XInputPoller::State ToState(const XINPUT_STATE& a_state)
		{
			return {
				.connected = true,
				.buttons = a_state.Gamepad.wButtons,
				.lx = NormalizeThumb(a_state.Gamepad.sThumbLX),
				.ly = NormalizeThumb(a_state.Gamepad.sThumbLY),
				.rx = NormalizeThumb(a_state.Gamepad.sThumbRX),
				.ry = NormalizeThumb(a_state.Gamepad.sThumbRY),
			};
		}

		constexpr DWORD kNoSlot = XUSER_MAX_COUNT;
		constexpr double kDiscoveryIntervalSeconds = 1.0;
	}

	XInputPoller::State XInputPoller::Poll(double a_now)
	{
		if (m_latchedSlot != kNoSlot) {
			XINPUT_STATE state{};
			if (XInputGetState(m_latchedSlot, &state) == ERROR_SUCCESS) {
				return ToState(state);
			}
			m_latchedSlot = kNoSlot;  // unplugged; fall through and rescan
		} else if (a_now < m_nextDiscoveryAt) {
			return {};
		}

		XInputPoller::State firstConnected{};
		for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
			XINPUT_STATE state{};
			if (XInputGetState(user, &state) != ERROR_SUCCESS) {
				continue;
			}
			if (ShowsActivity(state)) {
				m_latchedSlot = user;
				return ToState(state);
			}
			if (!firstConnected.connected) {
				firstConnected = ToState(state);
			}
		}
		// An idle connected pad must stay polled every update so its first press latches it.
		m_nextDiscoveryAt = firstConnected.connected ? 0.0 : a_now + kDiscoveryIntervalSeconds;
		return firstConnected;
	}

	void XInputPoller::Reset()
	{
		m_latchedSlot = kNoSlot;
		m_nextDiscoveryAt = 0.0;
	}
}
