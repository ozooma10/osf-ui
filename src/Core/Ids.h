#pragma once

#include <optional>

#include "Core/StringUtil.h"

namespace OSFUI::Ids
{
	// Mod ids are opaque, bounded Windows filename and URL path components.

	inline constexpr std::size_t kMaxModIdLen = 64;
	inline constexpr std::size_t kMaxViewNameLen = 64;

	constexpr std::string_view kBuiltInModId = "osfui";
	// These 1.x built-ins no longer exist. Keep the names reserved so stale packages
	// fail at discovery/registration instead of silently becoming ordinary web views.
	constexpr std::string_view kStaleSettingsViewId = "osfui/settings";
	constexpr std::string_view kStaleKeybindingsViewId = "osfui/keybinds";

	using StringUtil::EqualsCaseInsensitiveAscii;

	// One lowercase grammar segment: [a-z0-9-]+.
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

	// Reserve the platform id case-insensitively to prevent casing aliases.
	inline bool IsReservedModId(std::string_view a_id)
	{
		return EqualsCaseInsensitiveAscii(a_id, kBuiltInModId);
	}

	// These exclusions are filesystem, qualified-id, and virtual-host safety boundaries.
	inline bool IsValidModId(std::string_view a_id)
	{
		if (a_id.empty() || a_id.size() > kMaxModIdLen || IsReservedModId(a_id) ||
			a_id == "." || a_id == ".." || a_id.back() == '.' || a_id.back() == ' ') {
			return false;
		}
		for (const unsigned char c : a_id) {
			if (c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
				c == '\\' || c == '|' || c == '?' || c == '*' || c == '#' || c == '%') {
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

	// Only load-time validation accepts the canonical built-in id.
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
		return IsAcceptedModId(a_id.substr(0, slash)) && IsValidViewName(a_id.substr(slash + 1));
	}

	// Returned views alias a_id and must not outlive it.
	[[nodiscard]] inline std::string_view ModOf(std::string_view a_id) noexcept
	{
		const auto slash = a_id.find('/');
		return slash == std::string_view::npos ? a_id : a_id.substr(0, slash);
	}

	[[nodiscard]] inline std::string_view ViewNameOf(std::string_view a_id) noexcept
	{
		const auto slash = a_id.find('/');
		return slash == std::string_view::npos ? a_id : a_id.substr(slash + 1);
	}

}
