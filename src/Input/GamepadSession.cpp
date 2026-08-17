#include "Input/GamepadSession.h"

#include <array>
#include <cmath>

namespace OSFUI
{
	namespace
	{
		constexpr std::array<std::uint32_t, 14> kButtonMasks{
			XInputButton::kDPadUp,
			XInputButton::kDPadDown,
			XInputButton::kDPadLeft,
			XInputButton::kDPadRight,
			XInputButton::kStart,
			XInputButton::kBack,
			XInputButton::kLThumb,
			XInputButton::kRThumb,
			XInputButton::kLShoulder,
			XInputButton::kRShoulder,
			XInputButton::kA,
			XInputButton::kB,
			XInputButton::kX,
			XInputButton::kY,
		};

		GamepadSession::Action ButtonAction(std::uint32_t a_button)
		{
			switch (a_button)
			{
			case XInputButton::kDPadUp:
				return GamepadSession::Action::kUp;
			case XInputButton::kDPadDown:
				return GamepadSession::Action::kDown;
			case XInputButton::kDPadLeft:
				return GamepadSession::Action::kLeft;
			case XInputButton::kDPadRight:
				return GamepadSession::Action::kRight;
			case XInputButton::kA:
				return GamepadSession::Action::kActivate;
			case XInputButton::kB:
				return GamepadSession::Action::kBack;
			default:
				return GamepadSession::Action::kNone;
			}
		}

		GamepadSession::Action NavigationAction(std::uint8_t a_navigation)
		{
			switch (a_navigation)
			{
			case GamepadNavigation::kUp:
				return GamepadSession::Action::kUp;
			case GamepadNavigation::kDown:
				return GamepadSession::Action::kDown;
			case GamepadNavigation::kLeft:
				return GamepadSession::Action::kLeft;
			case GamepadNavigation::kRight:
				return GamepadSession::Action::kRight;
			default:
				return GamepadSession::Action::kNone;
			}
		}
	}

	GamepadSession::Frame GamepadSession::Update(const XInputPoller::State &a_state, bool a_defaultMapping, double a_deltaSeconds, double a_now) noexcept
	{
		Frame frame;
		frame.axes = {a_state.lx, a_state.ly, a_state.rx, a_state.ry};

		if (!m_active)
		{
			m_active = true;
			m_buttons = a_state.buttons;
		}
		else
		{
			const auto changed = m_buttons ^ a_state.buttons;
			for (const auto mask : kButtonMasks)
			{
				if ((changed & mask) == 0)
				{
					continue;
				}
				const bool down = (a_state.buttons & mask) != 0;
				frame.buttonEdges[frame.buttonEdgeCount++] = {
					mask,
					down,
					a_defaultMapping && down ? ButtonAction(mask) : Action::kNone};
			}
			m_buttons = a_state.buttons;
		}

		constexpr float kAxesPublishEpsilon = 0.04f;
		frame.axesChanged =
			std::fabs(frame.axes.lx - m_lastPublishedAxes.lx) > kAxesPublishEpsilon ||
			std::fabs(frame.axes.ly - m_lastPublishedAxes.ly) > kAxesPublishEpsilon ||
			std::fabs(frame.axes.rx - m_lastPublishedAxes.rx) > kAxesPublishEpsilon ||
			std::fabs(frame.axes.ry - m_lastPublishedAxes.ry) > kAxesPublishEpsilon;
		if (frame.axesChanged)
		{
			m_lastPublishedAxes = frame.axes;
		}

		if (!a_defaultMapping)
		{
			return frame;
		}

		frame.navigationAction = NavigationAction(m_navigation.Update(a_state.lx, a_state.ly, a_now));

		constexpr float kScrollDeadzone = 0.25f;
		if (std::fabs(a_state.ry) > kScrollDeadzone)
		{
			constexpr float kScrollNotchesPerSecond = 8.0f;
			m_scrollAccumulator += a_state.ry * kScrollNotchesPerSecond * static_cast<float>(a_deltaSeconds);
			const int notches = static_cast<int>(m_scrollAccumulator);
			if (notches != 0)
			{
				frame.wheelDelta = notches * 120;
				m_scrollAccumulator -= static_cast<float>(notches);
			}
		}
		else
		{
			m_scrollAccumulator = 0.0f;
		}

		return frame;
	}

	bool GamepadSession::End() noexcept
	{
		if (!m_active)
		{
			return false;
		}
		m_active = false;
		m_buttons = 0;
		m_navigation.Reset();
		m_scrollAccumulator = 0.0f;
		m_lastPublishedAxes = {};
		return true;
	}
}
