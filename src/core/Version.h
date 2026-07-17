#pragma once

namespace OSFUI
{
	inline constexpr const char* kPluginName = "OSF UI";
	inline constexpr const char* kPluginVersion = "1.0.0";
	// Numeric form of kPluginVersion, for the native plugin API
	// (IOSFUIBridge::GetPluginVersion). Keep in lockstep with kPluginVersion.
	inline constexpr std::uint32_t kPluginVersionMajor = 1;
	inline constexpr std::uint32_t kPluginVersionMinor = 0;
	inline constexpr std::uint32_t kPluginVersionPatch = 0;

	// Version of the native<->web bridge protocol (message envelope, command
	// whitelist, native->web message types). Distinct from kPluginVersion:
	// views negotiate against THIS via the `bridgeVersion` field in the
	// `runtime.ready` handshake. Keep in lockstep with
	// docs/authoring-views.md, docs/schema/*, and sdk/*.d.ts (CI enforces the
	// headline sites). 1.0 = STABLE: additive changes bump the minor and are
	// announced only through the append-only `capabilities` list; anything
	// that would break a shipped view bumps the major. Views feature-detect
	// via capabilities and must never parse this string.
	inline constexpr const char* kBridgeProtocolVersion = "1.0";

	// Name of the plugin data folder, resolved relative to the plugin DLL:
	//   Data/SFSE/Plugins/OSFUI/
	inline constexpr const char* kDataFolderName = "OSFUI";
}
