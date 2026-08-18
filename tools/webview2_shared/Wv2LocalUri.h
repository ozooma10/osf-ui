#pragma once

#include <string>

namespace osfui::wv2
{
	// Default-deny egress policy's single decision point: a URI is "local" iff
	// it is the view virtual host itself or a path under it, over http or
	// https. Everything else is answered locally with a synthesized 403 by
	// the host's WebResourceRequested filter. ASCII-only case folding is
	// correct here: scheme and host are ASCII per RFC 3986, and the virtual
	// host compared against is a fixed lowercase name. A host that merely
	// starts with the virtual host ("osfui.local.evil.com", a port, or a
	// userinfo trick) fails the base + "/" boundary check.
	//
	// Header-only and Windows-free so tests/native can exercise it directly.
	[[nodiscard]] inline bool IsLocalViewUri(std::wstring a_uri, const std::wstring& a_virtualHost)
	{
		for (auto& ch : a_uri) {
			if (ch >= L'A' && ch <= L'Z') ch += 32;
		}
		for (const auto* scheme : { L"https://", L"http://" }) {
			const auto base = scheme + a_virtualHost;
			if (a_uri == base || a_uri.starts_with(base + L"/")) return true;
		}
		return false;
	}
}
