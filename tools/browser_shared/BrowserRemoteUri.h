#pragma once

#include "BrowserLocalUri.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace osfui::browser
{
	namespace remote_uri
	{
		[[nodiscard]] inline bool IsDecimal(std::wstring_view text)
		{
			if (text.empty()) return false;
			for (const auto ch : text) if (ch < L'0' || ch > L'9') return false;
			return true;
		}

		// Accept only four decimal octets. Chromium also understands shortened,
		// integer, octal and hexadecimal IPv4; those spellings must fail closed
		// here rather than being mistaken for public DNS names.
		[[nodiscard]] inline std::optional<std::uint32_t> IPv4(std::wstring_view host)
		{
			std::uint32_t address{};
			for (unsigned i = 0; i < 4; ++i) {
				const auto end = host.find(L'.');
				const auto part = host.substr(0, end);
				if (!IsDecimal(part) || part.size() > 3 || (part.size() > 1 && part[0] == L'0')) return {};
				unsigned value{};
				for (const auto ch : part) value = value * 10 + (ch - L'0');
				if (value > 255 || (i == 3) != (end == std::wstring_view::npos)) return {};
				address = (address << 8) | value;
				if (end != std::wstring_view::npos) host.remove_prefix(end + 1);
			}
			return address;
		}

		[[nodiscard]] inline bool PublicIPv4(std::uint32_t ip)
		{
			const auto a = ip >> 24;
			const auto b = (ip >> 16) & 255;
			// Local, shared, link-local, benchmarking, multicast and reserved
			// ranges. This is a destination gate, not a DNS resolver/firewall.
			return a != 0 && a != 10 && a != 127 && a < 224 &&
				!(a == 100 && b >= 64 && b <= 127) &&
				!(a == 169 && b == 254) &&
				!(a == 172 && b >= 16 && b <= 31) &&
				!(a == 192 && b == 168) &&
				!(a == 198 && (b == 18 || b == 19));
		}

		using IPv6Address = std::array<std::uint16_t, 8>;

		[[nodiscard]] inline bool IPv6Parts(std::wstring_view text, IPv6Address& parts,
			std::size_t& count, bool allowIPv4)
		{
			if (text.empty()) return true;
			for (;;) {
				const auto end = text.find(L':');
				const auto part = text.substr(0, end);
				if (part.empty() || count == parts.size()) return false;
				if (part.find(L'.') != std::wstring_view::npos) {
					const auto ip = IPv4(part);
					if (!allowIPv4 || !ip || end != std::wstring_view::npos || count > 6) return false;
					parts[count++] = static_cast<std::uint16_t>(*ip >> 16);
					parts[count++] = static_cast<std::uint16_t>(*ip);
					return true;
				}
				if (part.size() > 4) return false;
				unsigned value{};
				for (const auto ch : part) {
					const auto lower = LowerAscii(ch);
					if (lower >= L'0' && lower <= L'9') value = value * 16 + (lower - L'0');
					else if (lower >= L'a' && lower <= L'f') value = value * 16 + (lower - L'a' + 10);
					else return false;
				}
				parts[count++] = static_cast<std::uint16_t>(value);
				if (end == std::wstring_view::npos) return true;
				text.remove_prefix(end + 1);
				if (text.empty()) return false;
			}
		}

		[[nodiscard]] inline std::optional<IPv6Address> IPv6(std::wstring_view host)
		{
			IPv6Address left{}, right{};
			std::size_t leftCount{}, rightCount{};
			const auto gap = host.find(L"::");
			if (gap == std::wstring_view::npos) {
				if (!IPv6Parts(host, left, leftCount, true) || leftCount != 8) return {};
			} else {
				if (host.find(L"::", gap + 2) != std::wstring_view::npos ||
					!IPv6Parts(host.substr(0, gap), left, leftCount, false) ||
					!IPv6Parts(host.substr(gap + 2), right, rightCount, true) ||
					leftCount + rightCount >= 8) return {};
				for (std::size_t i = 0; i < rightCount; ++i) left[8 - rightCount + i] = right[i];
			}
			return left;
		}

		[[nodiscard]] inline bool PublicIPv6(const IPv6Address& ip)
		{
			const auto tail = (std::uint32_t(ip[6]) << 16) | ip[7];
			// IPv4-mapped and the well-known NAT64 prefix retain the IPv4 gate.
			if (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0 && ip[4] == 0 && ip[5] == 0xffff)
				return PublicIPv4(tail);
			if (ip[0] == 0x64 && ip[1] == 0xff9b && ip[2] == 0 && ip[3] == 0 && ip[4] == 0 && ip[5] == 0)
				return PublicIPv4(tail);
			// Global unicast only: excludes loopback/unspecified, compatible IPv4,
			// ULA, site/link-local, multicast and local-use translation prefixes.
			if ((ip[0] & 0xe000) != 0x2000) return false;
			// Teredo embeds endpoints with additional transformations; do not
			// allow this transition mechanism. Check 6to4's embedded IPv4 too.
			if (ip[0] == 0x2001 && ip[1] == 0) return false;
			if (ip[0] == 0x2002 && !PublicIPv4((std::uint32_t(ip[1]) << 16) | ip[2])) return false;
			return true;
		}

		[[nodiscard]] inline bool HostIsOrUnder(std::wstring_view host, std::wstring_view suffix)
		{
			return host == suffix || (host.size() > suffix.size() && host.ends_with(suffix) &&
				host[host.size() - suffix.size() - 1] == L'.');
		}

		[[nodiscard]] inline bool PublicHost(std::wstring_view input)
		{
			std::wstring host;
			host.reserve(input.size());
			for (const auto ch : input) {
				if (ch > 0x7f) return false;  // IDNs must arrive in their ASCII/IDNA form.
				host.push_back(LowerAscii(ch));
			}
			if (host.ends_with(L'.')) host.pop_back();
			if (host.empty() || host.size() > 253) return false;
			for (const auto suffix : { kViewHost, std::wstring_view(L"localhost"), std::wstring_view(L"local"),
					 std::wstring_view(L"localdomain"), std::wstring_view(L"home.arpa"),
					 std::wstring_view(L"internal") }) {
				if (HostIsOrUnder(host, suffix)) return false;
			}
			if (const auto ip = IPv4(host)) return PublicIPv4(*ip);
			const std::wstring_view name(host);
			const auto lastDot = name.rfind(L'.');
			if (lastDot == std::wstring_view::npos) return false;  // No local search-domain expansion.
			const auto last = name.substr(lastDot + 1);
			// WHATWG "ends in a number" switches to IPv4 parsing. Only the
			// canonical four-octet form accepted above may take that path.
			if (IsDecimal(last) || last.starts_with(L"0x")) return false;
			for (std::size_t start = 0; start < name.size();) {
				const auto end = name.find(L'.', start);
				const auto label = name.substr(start, end == std::wstring_view::npos ? name.size() - start : end - start);
				if (label.empty() || label.size() > 63 || label.front() == L'-' || label.back() == L'-') return false;
				for (const auto ch : label)
					if (!((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') || ch == L'-')) return false;
				if (end == std::wstring_view::npos) return true;
				start = end + 1;
			}
			return false;  // A second trailing dot is not a valid hostname.
		}
	}

	// Absolute HTTP(S) destinations only. Intentionally stricter than Chromium:
	// reject credentials, encoded/non-ASCII hosts, backslashes, whitespace,
	// control characters and ambiguous IP notation instead of repairing them.
	// No DNS lookup is performed; a public hostname can resolve to private space.
	[[nodiscard]] inline bool IsAllowedRemoteNavigationUri(std::wstring_view uri)
	{
		for (const auto ch : uri) if (ch <= 0x20 || ch == 0x7f || ch == L'\\') return false;
		const auto colon = uri.find(L':');
		if (colon == std::wstring_view::npos ||
			(!EqualsAsciiCaseInsensitive(uri.substr(0, colon), L"http") &&
			 !EqualsAsciiCaseInsensitive(uri.substr(0, colon), L"https")) ||
			uri.substr(colon + 1, 2) != L"//") return false;
		const auto authority = uri.substr(colon + 3, uri.find_first_of(L"/?#", colon + 3) - (colon + 3));
		if (authority.empty() || authority.find_first_of(L"@%") != std::wstring_view::npos) return false;

		std::wstring_view port;
		bool hasPort{};
		bool hostAllowed{};
		if (authority.front() == L'[') {
			const auto close = authority.find(L']');
			if (close == std::wstring_view::npos) return false;
			const auto ip = remote_uri::IPv6(authority.substr(1, close - 1));
			hostAllowed = ip && remote_uri::PublicIPv6(*ip);
			if (close + 1 < authority.size()) {
				if (authority[close + 1] != L':') return false;
				hasPort = true;
				port = authority.substr(close + 2);
			}
		} else {
			const auto separator = authority.find(L':');
			hostAllowed = remote_uri::PublicHost(authority.substr(0, separator));
			if (separator != std::wstring_view::npos) {
				hasPort = true;
				port = authority.substr(separator + 1);
			}
		}
		if (!hostAllowed) return false;
		if (!hasPort) return true;
		if (!remote_uri::IsDecimal(port) || port.size() > 5) return false;
		unsigned value{};
		for (const auto ch : port) value = value * 10 + (ch - L'0');
		return value > 0 && value <= 65535;
	}
}
