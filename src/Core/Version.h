#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace OSFUI
{
	inline constexpr const char* kPluginName = "OSF UI";
	inline constexpr const char* kOsfuiReleaseVersion = "2.0.0";
	// Numeric form of the OSF UI release, exposed through the compatibility-named
	// IOSFUIBridge::GetPluginVersion method. Keep these in lockstep.
	inline constexpr std::uint32_t kOsfuiReleaseVersionMajor = 2;
	inline constexpr std::uint32_t kOsfuiReleaseVersionMinor = 0;
	inline constexpr std::uint32_t kOsfuiReleaseVersionPatch = 0;

	// Web bridge protocol version (message envelope, endpoint whitelist, and
	// OSF UI runtime-to-web message types). Distinct from the OSF UI release
	// version: views see this as the `bridgeVersion` field of the bridge ready
	// handshake. Keep in lockstep
	// with docs/authoring-views.md, docs/schema/*, and sdk/*.d.ts (CI enforces the
	// headline sites). Additive changes bump the minor, anything that breaks a
	// shipped view bumps the major — 2.0 is such a break (four verbs, envelopes
	// carrying routing beside the payload, a page-initiated handshake). Newer
	// targets are advisory; during 2.0.x explicitly pre-2.0 views are routed
	// through the isolated temporary compatibility façade and warned.
	inline constexpr const char* kBridgeProtocolVersion = "2.0";

	// "<major>[.<minor>[.<patch>]]", digits only — missing parts are 0.
	// Shared by every `targetVersion` site (view manifests, settings
	// schemas) so all of them accept exactly the same format.
	inline bool ParseDottedVersion(std::string_view a_text, std::array<std::uint32_t, 3>& a_out)
	{
		a_out = {};
		std::size_t part = 0;
		const char* pos = a_text.data();
		const char* end = a_text.data() + a_text.size();
		while (part < 3) {
			const auto [next, ec] = std::from_chars(pos, end, a_out[part]);
			if (ec != std::errc{} || next == pos) {
				return false;
			}
			pos = next;
			++part;
			if (pos == end) {
				return true;
			}
			if (*pos != '.') {
				return false;
			}
			++pos;
		}
		return false;  // more than three parts (or trailing dot)
	}

	// Numeric form of the running OSF UI release, for targetVersion comparisons.
	inline constexpr std::array<std::uint32_t, 3> kOsfuiReleaseVersionParts{
		kOsfuiReleaseVersionMajor, kOsfuiReleaseVersionMinor, kOsfuiReleaseVersionPatch
	};

	// True when a DECLARED `targetVersion` predates the 2.0 mod API — an
	// artifact authored against the 1.x helper and wire protocol. During 2.0.x
	// such a view receives the temporary façade and is reported through System
	// Health; the branch is removed in 2.1.0.
	// An UNDECLARED targetVersion ("") is deliberately excluded: it is
	// indistinguishable from "declared and unparsable" after parsing, and
	// guessing would badge every undeclared artifact as broken.
	[[nodiscard]] inline bool IsPre2Target(std::string_view a_target)
	{
		std::array<std::uint32_t, 3> parts{};
		return !a_target.empty() && ParseDottedVersion(a_target, parts) && parts[0] < 2;
	}

	[[nodiscard]] inline bool IsTargetNewerThanInstalledRelease(std::string_view a_target)
	{
		// The "needs update" condition behind Mod Settings. Empty or unparsable
		// targets are never newer.
		std::array<std::uint32_t, 3> parts{};
		return !a_target.empty() && ParseDottedVersion(a_target, parts) && kOsfuiReleaseVersionParts < parts;
	}

	// OSF UI's public Nexus Mods page, opened IN THE SYSTEM BROWSER by the
	// `osfui.openModPage` request endpoint. Hardcoded here (narrow twin for
	// logs/replies) so page content can only ever trigger this exact page —
	// never steer the shell to a URL of its choosing.
	inline constexpr const wchar_t* kNexusPageURLW = L"https://www.nexusmods.com/starfield/mods/17711";
	inline constexpr const char*    kNexusPageURL = "https://www.nexusmods.com/starfield/mods/17711";

	// Name of the plugin data folder, resolved relative to the plugin DLL:
	//   Data/SFSE/Plugins/OSFUI/
	inline constexpr const char* kDataFolderName = "OSFUI";
}
