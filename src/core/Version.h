#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace OSFUI
{
	inline constexpr const char* kPluginName = "OSF UI";
	inline constexpr const char* kPluginVersion = "1.2.0";
	// Numeric form of kPluginVersion, for IOSFUIBridge::GetPluginVersion.
	// Keep in lockstep with kPluginVersion.
	inline constexpr std::uint32_t kPluginVersionMajor = 1;
	inline constexpr std::uint32_t kPluginVersionMinor = 2;
	inline constexpr std::uint32_t kPluginVersionPatch = 0;

	// Native<->web bridge protocol version (message envelope, command whitelist,
	// native->web message types). Distinct from kPluginVersion: views see this as
	// the `bridgeVersion` field of the `runtime.ready` handshake. Keep in lockstep
	// with docs/authoring-views.md, docs/schema/*, and sdk/*.d.ts (CI enforces the
	// headline sites). Stable since 1.0: additive changes bump the minor, anything
	// that breaks a shipped view bumps the major. Compat is advisory, not gated —
	// an artifact declares `targetVersion` and the Mods surface badges
	// "needs update" against the host `version`.
	inline constexpr const char* kBridgeProtocolVersion = "1.2";

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

	// Numeric form of the running host version, for targetVersion compares.
	inline constexpr std::array<std::uint32_t, 3> kPluginVersionParts{
		kPluginVersionMajor, kPluginVersionMinor, kPluginVersionPatch
	};

	// OSF UI's public Nexus Mods page, opened IN THE SYSTEM BROWSER by the
	// `osfui.openModPage` web command. Hardcoded here (narrow twin for
	// logs/replies) so page content can only ever trigger this exact page —
	// never steer the shell to a URL of its choosing.
	inline constexpr const wchar_t* kNexusPageURLW = L"https://www.nexusmods.com/starfield/mods/17711";
	inline constexpr const char*    kNexusPageURL = "https://www.nexusmods.com/starfield/mods/17711";

	// Name of the plugin data folder, resolved relative to the plugin DLL:
	//   Data/SFSE/Plugins/OSFUI/
	inline constexpr const char* kDataFolderName = "OSFUI";
}
