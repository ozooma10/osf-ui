#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace osfui::wv2
{
	enum class SyntheticMouseButton : std::uint8_t
	{
		left = 0x01,
		right = 0x02,
		middle = 0x04
	};

	using SyntheticMouseButtonMask = std::uint8_t;

	inline constexpr std::array<SyntheticMouseButton, 3> kSyntheticMouseReleaseOrder{
		SyntheticMouseButton::left,
		SyntheticMouseButton::right,
		SyntheticMouseButton::middle};

	[[nodiscard]] constexpr SyntheticMouseButtonMask MouseButtonBit(SyntheticMouseButton a_button) noexcept
	{
		return static_cast<SyntheticMouseButtonMask>(a_button);
	}

	[[nodiscard]] constexpr std::string_view MouseButtonName(SyntheticMouseButton a_button) noexcept
	{
		switch (a_button)
		{
		case SyntheticMouseButton::left:
			return "left";
		case SyntheticMouseButton::right:
			return "right";
		case SyntheticMouseButton::middle:
			return "middle";
		}
		return "unknown";
	}

	class SyntheticMouseButtons
	{
	public:
		void Observe(SyntheticMouseButton a_button, bool a_down) noexcept
		{
			const auto bit = MouseButtonBit(a_button);
			if (a_down)
			{
				_pressed |= bit;
			}
			else
			{
				_pressed &= static_cast<SyntheticMouseButtonMask>(~bit);
			}
		}

		[[nodiscard]] bool IsPressed(SyntheticMouseButton a_button) const noexcept
		{
			return (_pressed & MouseButtonBit(a_button)) != 0;
		}

		[[nodiscard]] SyntheticMouseButtonMask Pressed() const noexcept
		{
			return _pressed;
		}

		// A non-zero result means the host crossed an input boundary without seeing the matching synthetic button-up. 
		// The caller must deliver forced releases to WebView2 before abandoning the old view/controller.
		[[nodiscard]] SyntheticMouseButtonMask TakePressed() noexcept
		{
			const auto pressed = _pressed;
			_pressed = 0;
			return pressed;
		}

	private:
		SyntheticMouseButtonMask _pressed{0};
	};
}
