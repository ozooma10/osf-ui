#pragma once

// ASCII-only string primitives shared across the plugin. ASCII is sufficient
// AND correct for every input these serve — grammar-constrained ids
// ([a-z0-9-]), key / enum identifiers, BCP-47 locale tags (alnum), and module
// basenames — all ASCII by construction. Kept deliberately locale-independent:
// the <cctype> equivalents depend on the active C locale, which nothing here
// sets. Header-only (inline), so any translation unit can include it freely.

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

	// ASCII lowercase copy (A-Z -> a-z; every other byte unchanged). Owning,
	// because lowercasing produces new characters.
	[[nodiscard]] inline std::string ToLowerAscii(std::string_view a_s)
	{
		std::string out(a_s);
		for (char& c : out) {
			c = detail::LowerAsciiChar(c);
		}
		return out;
	}

	// Narrow the view past leading/trailing ASCII whitespace — the same set as
	// std::isspace in the C locale: space, \t, \n, \v, \f, \r. Non-owning; wrap
	// with std::string(...) at sites that need to mutate or keep the result.
	[[nodiscard]] inline std::string_view TrimAscii(std::string_view a_s) noexcept
	{
		constexpr std::string_view kWs = " \t\n\v\f\r";
		const auto first = a_s.find_first_not_of(kWs);
		if (first == std::string_view::npos) {
			return {};
		}
		return a_s.substr(first, a_s.find_last_not_of(kWs) - first + 1);
	}

	// ASCII-only case-insensitive equality. ASCII suffices because callers
	// compare grammar-constrained ids / key / enum identifiers, where a script's
	// interned BSFixedString hands back the first-seen casing process-wide so the
	// literal spelling is unreliable (full rationale in api/SettingsMirror.h).
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
}
