#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "Input/GamepadNavigation.h"
#include "Input/XInputPoller.h"

namespace OSFUI::XInputButton
{
	// XInput wButtons masks; these numeric ids are also published by ui.gamepad.
	inline constexpr std::uint32_t kDPadUp = 0x0001;
	inline constexpr std::uint32_t kDPadDown = 0x0002;
	inline constexpr std::uint32_t kDPadLeft = 0x0004;
	inline constexpr std::uint32_t kDPadRight = 0x0008;
	inline constexpr std::uint32_t kStart = 0x0010;
	inline constexpr std::uint32_t kBack = 0x0020;
	inline constexpr std::uint32_t kLThumb = 0x0040;
	inline constexpr std::uint32_t kRThumb = 0x0080;
	inline constexpr std::uint32_t kLShoulder = 0x0100;
	inline constexpr std::uint32_t kRShoulder = 0x0200;
	inline constexpr std::uint32_t kA = 0x1000;
	inline constexpr std::uint32_t kB = 0x2000;
	inline constexpr std::uint32_t kX = 0x4000;
	inline constexpr std::uint32_t kY = 0x8000;
}

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
