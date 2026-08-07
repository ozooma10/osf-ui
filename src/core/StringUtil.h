#pragma once

// String primitives shared across the plugin. Two groups, deliberately kept
// apart:
//
//  * The ASCII group (ToLowerAscii / TrimAscii / EqualsCaseInsensitiveAscii).
//    ASCII is sufficient AND correct for every input these serve —
//    grammar-constrained ids ([a-z0-9-]), key / enum identifiers, and BCP-47
//    locale tags (alnum) — all ASCII by construction.
//  * The UTF-8 group (Utf8TruncateLen / TruncateUtf8 / SkipLeadingUtf8Continuations).
//    For *player- and author-supplied* text, which is arbitrary UTF-8. Use
//    these instead of a bare resize()/substr() on any string that later reaches
//    nlohmann::json::dump(): dump() defaults to error_handler_t::strict and
//    throws type_error.316 on an incomplete sequence, and most of our dump
//    sites sit on paths with no handler at all (the game thread's tick, the
//    browser host's wWinMain), where the throw is a std::terminate.
//
// Both groups are locale-independent: the <cctype> equivalents depend on the
// active C locale, which nothing here sets. Header-only (inline), so any
// translation unit can include it freely.

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

	namespace detail
	{
		// A UTF-8 continuation byte is 10xxxxxx. Lead bytes and ASCII are not.
		[[nodiscard]] constexpr bool IsUtf8Continuation(char a_c) noexcept
		{
			return (static_cast<unsigned char>(a_c) & 0xC0) == 0x80;
		}
	}

	// The largest length <= a_maxBytes that does not split a UTF-8 sequence.
	// Byte-identical to a_maxBytes for pure ASCII, so ASCII callers pay nothing.
	// Worst case backs off 3 bytes (the longest UTF-8 sequence is 4).
	[[nodiscard]] inline std::size_t Utf8TruncateLen(std::string_view a_s, std::size_t a_maxBytes) noexcept
	{
		if (a_s.size() <= a_maxBytes) {
			return a_s.size();
		}
		// a_s[n] is the first dropped byte. While it is a continuation byte the
		// sequence it belongs to straddles the cut, so retreat onto its lead byte.
		std::size_t n = a_maxBytes;
		while (n > 0 && detail::IsUtf8Continuation(a_s[n])) {
			--n;
		}
		return n;
	}

	// Shrink a_s to at most a_maxBytes bytes on a codepoint boundary. The drop-in
	// replacement for `if (s.size() > cap) { s.resize(cap); }`.
	inline void TruncateUtf8(std::string& a_s, std::size_t a_maxBytes)
	{
		a_s.resize(Utf8TruncateLen(a_s, a_maxBytes));
	}

	// Advance past leading continuation bytes. For a slice taken at an arbitrary
	// byte offset (a log tail), whose first bytes may be the remainder of a
	// sequence whose lead byte was cut away — the mirror of Utf8TruncateLen.
	[[nodiscard]] inline std::string_view SkipLeadingUtf8Continuations(std::string_view a_s) noexcept
	{
		std::size_t i = 0;
		while (i < a_s.size() && detail::IsUtf8Continuation(a_s[i])) {
			++i;
		}
		return a_s.substr(i);
	}
}
