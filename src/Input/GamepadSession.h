#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "Input/GamepadButtons.h"
#include "Input/GamepadNavigation.h"
#include "Input/XInputPoller.h"

namespace OSFUI
{
	class GamepadSession
	{
	public:
		enum class Action : std::uint8_t
		{
			kNone,
			kUp,
			kDown,
			kLeft,
			kRight,
			kActivate,
			kBack
		};

		struct ButtonEdge
		{
			std::uint32_t idCode{ 0 };
			bool          down{ false };
			Action        action{ Action::kNone };
		};

		struct Axes
		{
			float lx{ 0.0f };
			float ly{ 0.0f };
			float rx{ 0.0f };
			float ry{ 0.0f };
		};

		struct Frame
		{
			std::array<ButtonEdge, 14> buttonEdges{};
			std::size_t                buttonEdgeCount{ 0 };
			Axes                       axes{};
			bool                       axesChanged{ false };
			Action                     navigationAction{ Action::kNone };
			int                        wheelDelta{ 0 };
		};

		[[nodiscard]] Frame Update(const XInputPoller::State& a_state, bool a_defaultMapping, double a_deltaSeconds, double a_now) noexcept;

		[[nodiscard]] bool End() noexcept;
		[[nodiscard]] bool Active() const noexcept { return m_active; }

	private:
		bool              m_active{ false };
		std::uint32_t     m_buttons{ 0 };
		GamepadNavigation m_navigation;
		float             m_scrollAccumulator{ 0.0f };
		Axes              m_lastPublishedAxes{};
	};
}
