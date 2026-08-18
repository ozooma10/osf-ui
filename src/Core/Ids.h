#pragma once

#include <optional>

#include "Core/StringUtil.h"

namespace OSFUI::Ids
{
	// Mod ids are opaque names, not structured author/mod pairs. Dots have no
	// special meaning. The only syntax retained is the boundary required by the
	// places an id is used: a non-empty, bounded Windows filename / URL path
	// component. Qualified view ids still join that opaque id to the deliberately
	// narrow view-name grammar with '/'.

	inline constexpr std::size_t kMaxModIdLen = 64;
	inline constexpr std::size_t kMaxViewNameLen = 64;

	constexpr std::string_view kBuiltInModId = "osfui";
	constexpr std::string_view kSettingsViewId = "osfui/settings";
	constexpr std::string_view kKeybindingsViewId = "osfui/keybinds";

	constexpr std::string_view kToggleKey = "F10";  // the default key name the input layer listens for to open the default menu

	// ASCII-only case-insensitive equality, used by the Papyrus API to match
	// names and enum values (rationale in Core/StringUtil.h). Re-exported here so
	// the existing Ids::EqualsCaseInsensitiveAscii call sites read naturally.
	using StringUtil::EqualsCaseInsensitiveAscii;

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

	inline bool IsBuiltInModId(std::string_view a_id)
	{
		return a_id == kBuiltInModId;
	}

	// Windows treats this namespace case-insensitively. Reject aliases such as
	// "OSFUI" as well as the canonical spelling so a folder cannot squat the
	// platform id through casing.
	inline bool IsReservedModId(std::string_view a_id)
	{
		return EqualsCaseInsensitiveAscii(a_id, kBuiltInModId);
	}

	// Any non-empty Windows filename component, bounded because ids are echoed on
	// the wire and used as cache keys. The explicit exclusions are not a naming
	// grammar: they are the filesystem, qualified-id ('/'), and virtual-host URL
	// safety boundary. Dots, spaces, casing, underscores, and punctuation outside
	// that boundary are ordinary identity bytes.
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

	// Load-time acceptance includes the platform's canonical built-in id. Public
	// producer APIs use IsValidModId instead so third parties cannot claim it.
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

	// Split a qualified id "<modId>/<viewName>" on the first '/'. The results are
	// VIEWS into a_id — do not outlive it. A degenerate id with no '/' returns the
	// whole id from both, matching every caller's existing fallback.
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

	// --- Settings-write authority (docs/security-model.md) -------------------
	//
	// A settings write names its target mod in the payload. Left unchecked that
	// lets ANY view with the native bridge rewrite ANY installed mod's values —
	// including OSF UI's own `toggleKey`, which is the escape hatch the input
	// layer relies on to always be able to close the overlay.
	//
	// Only OSF UI's built-in Mod Settings and Keybindings views may name a foreign
	// mod: editing other mods' settings is their entire purpose. Every other view
	// is confined to its own mod, matching how current settings endpoints derive
	// authority from the source view rather than trusting the payload.
	//
	// Deliberately an exact qualified-id match rather than an `osfui/` prefix
	// test: reserving the mod id prevents third-party ownership, but only these
	// two built-in documents are settings editors. Mod Settings-only platform
	// requests use the same exact-id granularity.
	[[nodiscard]] inline bool IsSettingsEditorView(std::string_view a_viewId)
	{
		return a_viewId == kSettingsViewId || a_viewId == kKeybindingsViewId;
	}

	// The mod a settings write from a_sourceView is allowed to target, given the
	// payload's a_requestedMod. Returns std::nullopt when the request must be
	// refused (a non-editor view naming someone else's mod).
	//
	// An empty a_requestedMod from a non-editor view is resolved to its own mod
	// rather than refused, so a view may omit the field entirely — the field
	// carries no authority for them either way.
	[[nodiscard]] inline std::optional<std::string_view> ResolveWritableMod(
		std::string_view a_sourceView, std::string_view a_requestedMod)
	{
		if (IsSettingsEditorView(a_sourceView)) {
			return a_requestedMod;
		}
		const auto own = ModOf(a_sourceView);
		if (a_requestedMod.empty() || EqualsCaseInsensitiveAscii(a_requestedMod, own)) {
			return own;
		}
		return std::nullopt;
	}
}
