#pragma once

#include <cstdint>

namespace OSFUI
{
	// Converts the analogue left stick into one digital navigation direction.
	// A direction stays latched through release jitter, and only a deliberate
	// hold reaches repeat. Pure state keeps the policy independently testable.
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

			if (_activeDirection >= 0) {
				const auto active = static_cast<std::uint8_t>(_activeDirection);
				if (directed[active] > kReleaseThreshold) {
					if (a_now >= _nextRepeat) {
						_nextRepeat = a_now + kRepeatInterval;
						return static_cast<std::uint8_t>(1u << active);
					}
					return 0;
				}
				_activeDirection = -1;
				_nextRepeat = 0.0;
			}

			// Pick one axis so a diagonal deflection cannot inject two arrows in
			// the same frame. Ties favor vertical list navigation.
			const float absX = a_lx < 0.0f ? -a_lx : a_lx;
			const float absY = a_ly < 0.0f ? -a_ly : a_ly;
			if (absX < kEngageThreshold && absY < kEngageThreshold) {
				return 0;
			}

			if (absY >= absX) {
				_activeDirection = a_ly >= 0.0f ? 0 : 1;
			} else {
				_activeDirection = a_lx < 0.0f ? 2 : 3;
			}
			_nextRepeat = a_now + kInitialRepeatDelay;
			return static_cast<std::uint8_t>(1u << _activeDirection);
		}

		void Reset() noexcept
		{
			_activeDirection = -1;
			_nextRepeat = 0.0;
		}

	private:
		// The separate engage/release thresholds prevent a slowly returning or
		// slightly noisy stick from looking like several fresh presses.
		static constexpr float  kEngageThreshold = 0.55f;
		static constexpr float  kReleaseThreshold = 0.35f;
		static constexpr double kInitialRepeatDelay = 0.55;
		static constexpr double kRepeatInterval = 0.13;

		std::int8_t _activeDirection{ -1 };
		double      _nextRepeat{ 0.0 };
	};
}
