#pragma once
#include <cstdint>
#include <string>
#include <utility>

namespace osfui::browser
{
	class Utf16Input
	{
	public:
		void Reset() { m_high = 0; }
		std::u16string Push(char16_t c)
		{
			if (c >= 0xD800 && c <= 0xDBFF) { m_high = c; return {}; }
			const auto high = std::exchange(m_high, 0);
			if (c >= 0xDC00 && c <= 0xDFFF) {
				return high ? std::u16string{ high, c } : std::u16string{};
			}
			return std::u16string{ c };
		}
	private:
		char16_t m_high{ 0 };
	};
}
