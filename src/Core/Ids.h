#pragma once

#include "Core/StringUtil.h"

namespace OSFUI::Ids
{
	// Mod IDs must be safe in both URLs and Windows filenames.

	inline constexpr std::size_t kMaxModIdLen = 64;
	inline constexpr std::size_t kMaxViewNameLen = 64;

	constexpr std::string_view kBuiltInModId = "osfui";
	// Reserved so stale 1.x packages fail registration instead of loading as plain web views.
	constexpr std::string_view kStaleSettingsViewId = "osfui/settings";
	constexpr std::string_view kStaleKeybindingsViewId = "osfui/keybinds";

	using StringUtil::EqualsCaseInsensitiveAscii;

	inline bool IsValidSegment(std::string_view a_s)
	{
		if (a_s.empty()) {
			return false;
		}
		for (const char c : a_s) {
			const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
			if (!ok) {
				return false;
			}
		}
		return true;
	}

	inline bool IsBuiltInModId(std::string_view a_id)
	{
		return a_id == kBuiltInModId;
	}

	// Case-insensitive, so casing cannot bypass reserved names.
	inline bool IsReservedModId(std::string_view a_id)
	{
		return EqualsCaseInsensitiveAscii(a_id, kBuiltInModId);
	}

	// Ids double as filenames, qualified-id prefixes and virtual hosts.
	inline bool IsValidModId(std::string_view a_id)
	{
		if (a_id.empty() || a_id.size() > kMaxModIdLen || IsReservedModId(a_id) ||
			a_id == "." || a_id == ".." || a_id.back() == '.' || a_id.back() == ' ') {
			return false;
		}
		for (const unsigned char c : a_id) {
			if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~')) {
				return false;
			}
		}

		// Win32 device names remain reserved even with an extension ("NUL.json").
		const auto base = a_id.substr(0, a_id.find('.'));
		if (EqualsCaseInsensitiveAscii(base, "con") || EqualsCaseInsensitiveAscii(base, "prn") ||
			EqualsCaseInsensitiveAscii(base, "aux") || EqualsCaseInsensitiveAscii(base, "nul")) {
			return false;
		}
		if (base.size() == 4 && (EqualsCaseInsensitiveAscii(base.substr(0, 3), "com") ||
			EqualsCaseInsensitiveAscii(base.substr(0, 3), "lpt")) &&
			base[3] >= '1' && base[3] <= '9') {
			return false;
		}
		return true;
	}

	// Only load-time validation accepts the built-in ID.
	inline bool IsAcceptedModId(std::string_view a_id)
	{
		return IsValidModId(a_id) || IsBuiltInModId(a_id);
	}

	inline bool IsValidViewName(std::string_view a_name)
	{
		return a_name.size() <= kMaxViewNameLen && IsValidSegment(a_name);
	}

	// "<modId>/<viewName>" — the only shape RegisterView / menu targets accept.
	inline bool IsValidQualifiedViewId(std::string_view a_id)
	{
		if (EqualsCaseInsensitiveAscii(a_id, kStaleSettingsViewId) ||
			EqualsCaseInsensitiveAscii(a_id, kStaleKeybindingsViewId)) {
			return false;
		}
		const auto slash = a_id.find('/');
		if (slash == std::string_view::npos || a_id.find('/', slash + 1) != std::string_view::npos) {
			return false;
		}
		return IsAcceptedModId(a_id.substr(0, slash)) && IsValidViewName(StringUtil::ToLowerAscii(a_id.substr(slash + 1)));
	}

	// Returned views alias a_id and must not outlive it.
	[[nodiscard]] inline std::string_view ModOf(std::string_view a_id) noexcept
	{
		const auto slash = a_id.find('/');
		return slash == std::string_view::npos ? a_id : a_id.substr(0, slash);
	}

}
