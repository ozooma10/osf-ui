#pragma once

namespace OSFUI::Ids
{
	// The public id grammar (docs/api-freeze-plan.md item 1, frozen pre-1.0):
	//
	//   mod id        = <author>.<modname>     e.g. "ozooma10.almanac"
	//   view name     = <name>                 e.g. "planets"
	//   qualified view = <modId>/<viewName>    e.g. "ozooma10.almanac/planets"
	//
	// Every segment is lowercase [a-z0-9-]+. A mod id contains EXACTLY one dot,
	// which makes dotless ids platform-reserved by construction: built-ins
	// ("osfui") may be dotless, third parties may not — no reserved-word list
	// to maintain. The qualified join uses a slash because mod ids contain a
	// dot (a dotted join would be ambiguous to split); the slash also mirrors
	// the on-disk layout views/<modId>/<viewName>/.
	//
	// Ids become filenames (settings/<modId>.json) and sandbox URL segments
	// (file:///<modId>/<viewName>/...), so the charset doubles as the
	// path-safety boundary (docs/security-model.md).

	inline constexpr std::size_t kMaxModIdLen = 64;
	inline constexpr std::size_t kMaxViewNameLen = 64;

	// ASCII-only case-insensitive equality. The Papyrus surface matches names
	// and enum values with this: script strings intern as BSFixedString, which
	// hands back the FIRST-seen casing process-wide, so a script's literal
	// spelling never survives reliably (api/SettingsMirror.h has the full
	// rationale). Sufficient because ids are lowercase [a-z0-9-] by grammar
	// and keys/options are ASCII identifiers in practice.
	inline bool EqualsCaseInsensitiveAscii(std::string_view a_lhs, std::string_view a_rhs)
	{
		if (a_lhs.size() != a_rhs.size()) {
			return false;
		}
		for (std::size_t i = 0; i < a_lhs.size(); ++i) {
			const char a = a_lhs[i];
			const char b = a_rhs[i];
			const char la = (a >= 'A' && a <= 'Z') ? static_cast<char>(a + 32) : a;
			const char lb = (b >= 'A' && b <= 'Z') ? static_cast<char>(b + 32) : b;
			if (la != lb) {
				return false;
			}
		}
		return true;
	}

	// One grammar segment: [a-z0-9-]+ (lowercase enforced at load, so
	// case-sensitive compares are correct on case-insensitive filesystems).
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

	// Dotless ids the platform itself ships under. Everything else dotless is
	// invalid by the grammar, so no further reservation logic is needed.
	inline bool IsBuiltInModId(std::string_view a_id)
	{
		return a_id == "osfui";
	}

	// <author>.<modname> — exactly one dot, both segments valid.
	inline bool IsValidModId(std::string_view a_id)
	{
		if (a_id.size() > kMaxModIdLen) {
			return false;
		}
		const auto dot = a_id.find('.');
		if (dot == std::string_view::npos || a_id.find('.', dot + 1) != std::string_view::npos) {
			return false;
		}
		return IsValidSegment(a_id.substr(0, dot)) && IsValidSegment(a_id.substr(dot + 1));
	}

	// A third-party or built-in mod id (the load-time acceptance test).
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
		const auto slash = a_id.find('/');
		if (slash == std::string_view::npos || a_id.find('/', slash + 1) != std::string_view::npos) {
			return false;
		}
		return IsAcceptedModId(a_id.substr(0, slash)) && IsValidViewName(a_id.substr(slash + 1));
	}
}
