#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace osfui::wv2
{
	inline constexpr std::wstring_view kBuiltInViewHost = L"osfui.example";
	inline constexpr std::wstring_view kSharedAssetHost = L"osfui-assets.example";
	inline constexpr std::array<std::wstring_view, 3> kSharedAssetPaths{
		L"/osfui.js", L"/osfui.css", L"/gamepadnav.js"
	};

	struct HttpsUri
	{
		std::wstring host;
		std::wstring path;
	};

	[[nodiscard]] inline wchar_t LowerAscii(const wchar_t a_ch) noexcept
	{
		return a_ch >= L'A' && a_ch <= L'Z' ? a_ch + (L'a' - L'A') : a_ch;
	}

	[[nodiscard]] inline bool EqualsAsciiCaseInsensitive(std::wstring_view a_left, std::wstring_view a_right) noexcept
	{
		if (a_left.size() != a_right.size()) return false;
		for (std::size_t i = 0; i < a_left.size(); ++i) {
			if (LowerAscii(a_left[i]) != LowerAscii(a_right[i])) return false;
		}
		return true;
	}

	[[nodiscard]] inline bool IsSafeVirtualPath(std::wstring_view a_path)
	{
		std::wstring lowered;
		lowered.reserve(a_path.size());
		for (const auto ch : a_path) lowered.push_back(LowerAscii(ch));
		if (lowered.find(L"%2f") != std::wstring::npos ||
			lowered.find(L"%5c") != std::wstring::npos) {
			return false;
		}
		std::size_t start = 0;
		while (start <= lowered.size()) {
			const auto end = lowered.find(L'/', start);
			auto segment = lowered.substr(start,
				end == std::wstring::npos ? std::wstring::npos : end - start);
			for (std::size_t at = 0; (at = segment.find(L"%2e", at)) != std::wstring::npos;) {
				segment.replace(at, 3, L".");
				++at;
			}
			if (segment == L"." || segment == L"..") return false;
			if (end == std::wstring::npos) break;
			start = end + 1;
		}
		return true;
	}

	// Parse only the virtual HTTPS URLs OSF UI creates. Explicit ports, userinfo, backslashes, and non-HTTPS schemes are rejected rather than normalized.
	[[nodiscard]] inline std::optional<HttpsUri> ParseHttpsUri(std::wstring_view a_uri)
	{
		constexpr std::wstring_view scheme = L"https://";
		if (a_uri.size() < scheme.size() || !EqualsAsciiCaseInsensitive(a_uri.substr(0, scheme.size()), scheme) || a_uri.find(L'\\') != std::wstring_view::npos) {
			return std::nullopt;
		}

		const auto authorityStart = scheme.size();
		const auto authorityEnd = a_uri.find_first_of(L"/?#", authorityStart);
		const auto authority = a_uri.substr(authorityStart, authorityEnd == std::wstring_view::npos ? std::wstring_view::npos : authorityEnd - authorityStart);
		if (authority.empty() || authority.find_first_of(L"@:") != std::wstring_view::npos) {
			return std::nullopt;
		}

		HttpsUri parsed;
		parsed.host.reserve(authority.size());
		for (const auto ch : authority) parsed.host.push_back(LowerAscii(ch));

		if (authorityEnd == std::wstring_view::npos || a_uri[authorityEnd] != L'/') {
			parsed.path = L"/";
		} else {
			const auto pathEnd = a_uri.find_first_of(L"?#", authorityEnd);
			parsed.path.assign(a_uri.substr(authorityEnd, pathEnd == std::wstring_view::npos ? std::wstring_view::npos : pathEnd - authorityEnd));
		}
		return parsed;
	}

	[[nodiscard]] inline bool IsHttpsUriForHost(std::wstring_view a_uri, std::wstring_view a_host)
	{
		const auto parsed = ParseHttpsUri(a_uri);
		return parsed && IsSafeVirtualPath(parsed->path) && EqualsAsciiCaseInsensitive(parsed->host, a_host);
	}

	[[nodiscard]] inline bool IsTrustedViewDocumentUri(std::wstring_view a_uri, std::wstring_view a_host, std::wstring_view a_viewName)
	{
		const auto parsed = ParseHttpsUri(a_uri);
		if (!parsed || !IsSafeVirtualPath(parsed->path) || !EqualsAsciiCaseInsensitive(parsed->host, a_host) || a_viewName.empty()) {
			return false;
		}
		const auto root = std::wstring(L"/") + std::wstring(a_viewName);
		return parsed->path == root || parsed->path.starts_with(root + L"/");
	}

	[[nodiscard]] inline bool IsSharedAssetUri(std::wstring_view a_uri)
	{
		const auto parsed = ParseHttpsUri(a_uri);
		if (!parsed || !IsSafeVirtualPath(parsed->path) || !EqualsAsciiCaseInsensitive(parsed->host, kSharedAssetHost)) {
			return false;
		}
		for (const auto path : kSharedAssetPaths) {
			if (parsed->path == path) return true;
		}
		return false;
	}

	// Published views used ../../shared/<asset> while every view shared one virtual host. 
	// Dedicated per-mod hosts resolve that spelling inside the mod instead.
	// Return the canonical shared-origin URL for only the three public assets.
	[[nodiscard]] inline std::optional<std::wstring> LegacySharedAssetTargetUri(
		std::wstring_view a_uri, std::wstring_view a_viewHost)
	{
		const auto parsed = ParseHttpsUri(a_uri);
		if (!parsed || !IsSafeVirtualPath(parsed->path) ||
			!EqualsAsciiCaseInsensitive(parsed->host, a_viewHost)) {
			return std::nullopt;
		}

		constexpr std::wstring_view prefix = L"/shared";
		if (!parsed->path.starts_with(prefix)) return std::nullopt;
		const auto assetPath = std::wstring_view(parsed->path).substr(prefix.size());
		bool allowed = false;
		for (const auto path : kSharedAssetPaths) {
			if (assetPath == path) {
				allowed = true;
				break;
			}
		}
		if (!allowed) return std::nullopt;

		std::wstring target = L"https://" + std::wstring(kSharedAssetHost) + std::wstring(assetPath);
		if (const auto suffix = a_uri.find_first_of(L"?#", std::wstring_view(L"https://").size());
			suffix != std::wstring_view::npos) {
			target.append(a_uri.substr(suffix));
		}
		return target;
	}

	[[nodiscard]] inline bool IsAllowedViewResourceUri(std::wstring_view a_uri, std::wstring_view a_viewHost)
	{
		return IsHttpsUriForHost(a_uri, a_viewHost) || IsSharedAssetUri(a_uri);
	}

	[[nodiscard]] inline bool IsAllowedBlankFrameUri(std::wstring_view a_uri) noexcept
	{
		return EqualsAsciiCaseInsensitive(a_uri, L"about:blank") || EqualsAsciiCaseInsensitive(a_uri, L"about:srcdoc");
	}
}
