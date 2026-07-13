// ============================================================================
// OSFUI_API.h - OSF UI native bridge API.
// Copyable SINGLE header. Drop it into your SFSE plugin; link NOTHING.
//
// It lets a separate SFSE plugin register bridge commands, push data to a web
// view, and receive commands back from it, WITHOUT compiling its code into
// OSFUI.dll. See docs/native-plugin-api.md in the OSF UI repo.
//
// THREADING:
//   Status reads (GetInterfaceVersion/GetPluginVersion/GetBridgeProtocolVersion/
//   IsBridgeReady) are callable from ANY thread.
//   Mutating calls (RegisterCommand/UnregisterCommand/SetReadyCallback/SendToWeb)
//   are thread-safe; their effect lands on the game main thread.
//   Typed setting getters (GetSettingBool/Int/Float/String) are synchronous
//   and callable from ANY thread.
//   CommandFn, ReadyFn and SettingChangedFn ALWAYS run on the game main
//   thread - keep them cheap.
//
// ABI: the surface carries only primitives, UTF-8 const char*, function
//   pointers and void* user data - no STL, no nlohmann::json, no RE::* types.
//   It is therefore independent of the CommonLibSF pin. MAJOR breaks ABI; MINOR
//   appends to the end of the vtable (older callers keep working).
// ============================================================================
#pragma once

#include "REX/W32/KERNEL32.h"  // GetModuleHandleW / GetProcAddress / HMODULE (no <Windows.h>)
#include <cstdint>

namespace OSFUI::API
{
	// Packed (MAJOR << 16) | MINOR. MAJOR breaks ABI; MINOR bumps on an appended vmethod.
	// History: 1.0 commands/sends/ready; 1.1 +RequestMenu; 1.2 +settings
	// (SubscribeSettings, typed getters, RegisterSettingsSchema).
	inline constexpr std::uint32_t kBridgeAPIVersion = (1u << 16) | 2u;
	inline constexpr std::uint32_t kBridgeAPIMajor   = kBridgeAPIVersion >> 16;
	inline constexpr std::uint32_t kBridgeAPIMinor   = kBridgeAPIVersion & 0xFFFFu;

	inline constexpr const wchar_t* kModuleName        = L"OSFUI.dll";
	inline constexpr const char*    kRequestExportName = "OSFUI_RequestBridge";

	// Handler for one registered ui.command. Runs on the GAME (main) thread.
	//   a_command      : the command string registered (lets one fn serve many)
	//   a_payloadJson  : the command payload object, serialized - e.g. "{\"id\":\"x\"}"
	//   a_sourceViewId : the view that sent it (your reply target)
	//   a_user         : the opaque pointer you passed to RegisterCommand
	using CommandFn = void (*)(const char* a_command,
	                           const char* a_payloadJson,
	                           const char* a_sourceViewId,
	                           void*       a_user) noexcept;

	// Fired on the GAME (main) thread when the bridge becomes ready (a
	// nativeBridge view is live) and again after any re-creation.
	using ReadyFn = void (*)(void* a_user) noexcept;

	// Fired on the GAME (main) thread for every committed value of a mod
	// subscribed via SubscribeSettings. a_valueJson is the value as serialized
	// JSON text - e.g. "true", "1.5", "\"compact\"".
	using SettingChangedFn = void (*)(const char* a_modId,
	                                  const char* a_key,
	                                  const char* a_valueJson,
	                                  void*       a_user) noexcept;

	struct IOSFUIBridge
	{
		// --- versioning / status. ANY thread, synchronous. ---
		virtual std::uint32_t GetInterfaceVersion() = 0;
		virtual void          GetPluginVersion(std::uint32_t& a_major,
		                                       std::uint32_t& a_minor,
		                                       std::uint32_t& a_patch) = 0;
		virtual const char*   GetBridgeProtocolVersion() = 0;  // web protocol, e.g. "0.1"
		virtual bool          IsBridgeReady() = 0;             // a nativeBridge view is live

		// --- command registration. Thread-safe; applied on the next main tick. ---
		// Register/replace the handler for an EXACT command string (e.g. "osf.launch").
		// Persists across bridge re-creation. Reserved prefixes are refused:
		// ui. / runtime. / game. / settings. Use your own namespace.
		virtual void RegisterCommand(const char* a_command, CommandFn a_handler, void* a_user) = 0;
		virtual void UnregisterCommand(const char* a_command) = 0;

		// --- native -> web. Thread-safe; queued to the target view. ---
		// Delivers { "type": a_type, "payload": <a_payloadJson> } to a_viewId.
		// a_payloadJson must be valid JSON text. Returns false if the bridge is
		// down or the payload won't parse (message dropped).
		virtual bool SendToWeb(const char* a_viewId, const char* a_type, const char* a_payloadJson) = 0;

		// --- readiness notification. Callback runs on the main thread. ---
		virtual void SetReadyCallback(ReadyFn a_callback, void* a_user) = 0;

		// --- menu control Thread-safe; applied on the next main tick. ---
		// Open or close a registered SURFACE (menu/HUD view) by id
		// the request is marshaled onto the main thread and run through the normal menu policy.
		// Lets a sibling plugin surface its own view (e.g. an in-game item that opens the scene browser). No-op if the id is not a registered surface.
		virtual bool RequestMenu(const char* a_viewId, bool a_open) = 0;

		// ===== settings consumption (MINOR 1.2 block, mcm-design.md §8.2) =====

		// --- change subscription. Thread-safe; callbacks on the GAME MAIN thread. ---
		// Fires for every committed value of a_modId, and is REPLAYED once per
		// current value on subscribe (and again if the mod registers later -
		// subscribing to a not-yet-registered id is legal; the replay arrives
		// when it registers). Per-mod, not per-key: one subscription, switch on
		// a_key. Returns a token for UnsubscribeSettings; 0 on null a_modId/a_fn.
		virtual std::uint32_t SubscribeSettings(const char* a_modId, SettingChangedFn a_fn, void* a_user) = 0;
		virtual void          UnsubscribeSettings(std::uint32_t a_token) = 0;

		// --- typed getters. Synchronous, callable from ANY thread - they read a
		// mutex-guarded value mirror maintained on the main thread, never the
		// store. false / 0 on unknown mod/key or type mismatch. ---
		virtual bool GetSettingBool(const char* a_modId, const char* a_key, bool* a_out) = 0;
		virtual bool GetSettingInt(const char* a_modId, const char* a_key, std::int64_t* a_out) = 0;
		virtual bool GetSettingFloat(const char* a_modId, const char* a_key, double* a_out) = 0;
		// Covers string, enum (the option string), and key (the key NAME, e.g.
		// "F10"). Returns the required length INCLUDING the NUL (0 on unknown/
		// mismatch); copies min(a_bufLen) bytes, always NUL-terminated when
		// a_bufLen > 0. A null/empty buffer is the "how big?" probe.
		virtual std::uint32_t GetSettingString(const char* a_modId, const char* a_key,
		                                       char* a_buf, std::uint32_t a_bufLen) = 0;

		// --- settings registration. Thread-safe; the merge lands on the next
		// main tick. ---
		// a_schemaJson is the SAME JSON document that would live in a
		// settings/<id>.json drop-in (docs/schema in the OSF UI repo). Returns
		// false synchronously on a parse/shape error: malformed JSON, a
		// non-object document, or a missing/invalid/reserved "id" (deeper field
		// problems fall back defensively, exactly like a drop-in file). true =
		// accepted and queued. Persisted user values overlay from the same
		// per-mod values file as the drop-in tier, so a mod can migrate tiers
		// without losing settings. Same id as a drop-in file: this registration
		// wins (warning in the log); same id as an earlier runtime registration:
		// replaced (dev iteration). SubscribeSettings consumers of the id
		// receive the value replay when the merge commits.
		virtual bool RegisterSettingsSchema(const char* a_schemaJson) = 0;
		// Drops a schema registered through RegisterSettingsSchema (the user's
		// values file on disk is kept). Ignored, with a warning, for ids owned
		// by drop-in files.
		virtual void UnregisterSettingsSchema(const char* a_modId) = 0;

	protected:
		~IOSFUIBridge() = default;  // OSF UI owns the singleton; the consumer never deletes it.
	};

	using RequestBridge_t = IOSFUIBridge* (*)(std::uint32_t a_abiVersion) noexcept;

	// FETCH ONCE and cache. Call after SFSE kPostLoad. Do NOT call per-frame.
	// Returns nullptr if OSF UI is absent or its MAJOR differs from yours - a
	// normal, expected outcome; degrade (no UI) rather than failing.
	//
	// FEATURE DETECTION: an OLDER 1.x OSFUI.dll still hands you a bridge whose
	// vtable ends before the newest methods. Before calling anything a later
	// MINOR added (see the History note above), check the runtime's version:
	//   if ((bridge->GetInterfaceVersion() & 0xFFFF) >= 2) { /* settings ok */ }
	inline IOSFUIBridge* RequestBridge(std::uint32_t a_abiVersion = kBridgeAPIVersion) noexcept
	{
		const REX::W32::HMODULE mod = REX::W32::GetModuleHandleW(kModuleName);
		if (!mod) {
			return nullptr;  // OSF UI not installed/loaded.
		}
		const auto fn = reinterpret_cast<RequestBridge_t>(
			REX::W32::GetProcAddress(mod, kRequestExportName));
		return fn ? fn(a_abiVersion) : nullptr;  // older OSF UI / MAJOR mismatch -> nullptr.
	}
}
