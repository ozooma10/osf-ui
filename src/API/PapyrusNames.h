#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace OSFUI::PapyrusNames
{
	inline constexpr std::size_t kMaxTargetLen = 128;

	inline bool IsIdentifier(std::string_view a_name)
	{
		if (a_name.empty() || a_name.size() > kMaxTargetLen) {
			return false;
		}
		const auto isAlpha = [](char c) {
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
		};
		const auto isAlnum = [&](char c) {
			return isAlpha(c) || (c >= '0' && c <= '9');
		};
		return isAlpha(a_name.front()) &&
		       std::all_of(a_name.begin() + 1, a_name.end(), isAlnum);
	}

	// Papyrus script names may contain namespace-separated identifiers.
	inline bool IsScriptName(std::string_view a_name)
	{
		if (a_name.empty() || a_name.size() > kMaxTargetLen) {
			return false;
		}
		std::size_t start = 0;
		while (start < a_name.size()) {
			const auto end = a_name.find(':', start);
			const auto part = a_name.substr(start,
				end == std::string_view::npos ? a_name.size() - start : end - start);
			if (!IsIdentifier(part)) {
				return false;
			}
			if (end == std::string_view::npos) {
				return true;
			}
			start = end + 1;
		}
		return false;
	}
}
