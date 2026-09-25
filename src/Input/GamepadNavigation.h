#pragma once

#include <cstdint>

namespace OSFUI
{
	// Convert the analogue stick to one latched digital direction with deliberate hold repeat.
	class GamepadNavigation
	{
	public:
		enum Direction : std::uint8_t
		{
			kUp = 1u << 0,
			kDown = 1u << 1,
			kLeft = 1u << 2,
			kRight = 1u << 3,
		};

		[[nodiscard]] std::uint8_t Update(float a_lx, float a_ly, double a_now) noexcept
		{
			const float directed[4] = { a_ly, -a_ly, -a_lx, a_lx };

			if (m_activeDirection >= 0) {
				const auto active = static_cast<std::uint8_t>(m_activeDirection);
				if (directed[active] > kReleaseThreshold) {
					if (a_now >= m_nextRepeat) {
						m_nextRepeat = a_now + kRepeatInterval;
						return static_cast<std::uint8_t>(1u << active);
					}
					return 0;
				}
				m_activeDirection = -1;
				m_nextRepeat = 0.0;
			}

			// Choose one axis per frame; diagonal ties favor vertical navigation.
			const float absX = a_lx < 0.0f ? -a_lx : a_lx;
			const float absY = a_ly < 0.0f ? -a_ly : a_ly;
			if (absX < kEngageThreshold && absY < kEngageThreshold) {
				return 0;
			}

			if (absY >= absX) {
				m_activeDirection = a_ly >= 0.0f ? 0 : 1;
			} else {
				m_activeDirection = a_lx < 0.0f ? 2 : 3;
			}
			m_nextRepeat = a_now + kInitialRepeatDelay;
			return static_cast<std::uint8_t>(1u << m_activeDirection);
		}

		void Reset() noexcept
		{
			m_activeDirection = -1;
			m_nextRepeat = 0.0;
		}

	private:
		// Separate engage and release thresholds suppress noisy repeat presses.
		static constexpr float  kEngageThreshold = 0.55f;
		static constexpr float  kReleaseThreshold = 0.35f;
		static constexpr double kInitialRepeatDelay = 0.55;
		static constexpr double kRepeatInterval = 0.13;

		std::int8_t m_activeDirection{ -1 };
		double      m_nextRepeat{ 0.0 };
	};
}
