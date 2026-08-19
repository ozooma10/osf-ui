#pragma once

#include <string>

namespace osfui::wv2
{
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
