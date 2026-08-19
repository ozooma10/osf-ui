#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace OSFUI::StringUtil
{
	namespace detail
	{
		[[nodiscard]] constexpr char LowerAsciiChar(char a_c) noexcept
		{
			return (a_c >= 'A' && a_c <= 'Z') ? static_cast<char>(a_c + 32) : a_c;
		}
	}

	// Owning ASCII lowercase copy; non-A-Z bytes are unchanged.
	[[nodiscard]] inline std::string ToLowerAscii(std::string_view a_s)
	{
		std::string out(a_s);
		for (char& c : out) {
			c = detail::LowerAsciiChar(c);
		}
		return out;
	}

	[[nodiscard]] inline std::string_view TrimAscii(std::string_view a_s) noexcept
	{
		constexpr std::string_view kWs = " \t\n\v\f\r";
		const auto first = a_s.find_first_not_of(kWs);
		if (first == std::string_view::npos) {
			return {};
		}
		return a_s.substr(first, a_s.find_last_not_of(kWs) - first + 1);
	}

	[[nodiscard]] inline bool EqualsCaseInsensitiveAscii(std::string_view a_lhs, std::string_view a_rhs) noexcept
	{
		if (a_lhs.size() != a_rhs.size()) {
			return false;
		}
		for (std::size_t i = 0; i < a_lhs.size(); ++i) {
			if (detail::LowerAsciiChar(a_lhs[i]) != detail::LowerAsciiChar(a_rhs[i])) {
				return false;
			}
		}
		return true;
	}

	namespace detail
	{
		// A UTF-8 continuation byte is 10xxxxxx. Lead bytes and ASCII are not.
		[[nodiscard]] constexpr bool IsUtf8Continuation(char a_c) noexcept
		{
			return (static_cast<unsigned char>(a_c) & 0xC0) == 0x80;
		}
	}

	[[nodiscard]] inline std::size_t Utf8TruncateLen(std::string_view a_s, std::size_t a_maxBytes) noexcept
	{
		if (a_s.size() <= a_maxBytes) {
			return a_s.size();
		}
		std::size_t n = a_maxBytes;
		while (n > 0 && detail::IsUtf8Continuation(a_s[n])) {
			--n;
		}
		return n;
	}

	inline void TruncateUtf8(std::string& a_s, std::size_t a_maxBytes)
	{
		a_s.resize(Utf8TruncateLen(a_s, a_maxBytes));
	}

	[[nodiscard]] inline std::string_view SkipLeadingUtf8Continuations(std::string_view a_s) noexcept
	{
		std::size_t i = 0;
		while (i < a_s.size() && detail::IsUtf8Continuation(a_s[i])) {
			++i;
		}
		return a_s.substr(i);
	}
}
