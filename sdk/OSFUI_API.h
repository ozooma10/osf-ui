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
//   Mutating calls (RegisterCommand/UnregisterCommand/SetReadyCallback/SendToWeb/
//   RequestMenu/RegisterView) are thread-safe; their effect lands on the game
//   main thread.
//   Typed setting getters (GetSettingBool/Int/Float/String) are synchronous
//   and callable from ANY thread.
//   CommandFn, ReadyFn, SettingChangedFn and HotkeyFn ALWAYS run on the game
//   main thread - keep them cheap.
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
	// Packed (MAJOR << 16) | MINOR. MAJOR breaks ABI; MINOR bumps on an appended
	// vmethod — or, as of 1.3, on a strengthened behavioral guarantee a consumer
	// may need to detect (no vtable change).
	// History: 1.0 commands/sends/ready; 1.1 +RequestMenu; 1.2 +settings
	// (SubscribeSettings, typed getters, RegisterSettingsSchema); 1.3 SendToWeb
	// delivery guarantee (queue-until-deliverable + message-before-first-paint;
	// see SendToWeb below — no new vmethods); 1.4 +hotkeys (SubscribeHotkey/
	// UnsubscribeHotkey — every key-typed setting is a dispatchable binding);
	// 1.5 +RegisterView (load + surface-register a views/<id>/ folder your mod
	// ships, without the user's config.json listing it).
	inline constexpr std::uint32_t kBridgeAPIVersion = (1u << 16) | 5u;
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

	// Fired on the GAME (main) thread when the physical key currently bound to
	// the key-typed setting subscribed via SubscribeHotkey is pressed (1.4).
	using HotkeyFn = void (*)(const char* a_modId,
	                          const char* a_key,
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
		// a_payloadJson must be valid JSON text.
		//
		// DELIVERY GUARANTEE (MINOR >= 3): a message to a loaded, bridge-enabled
		// view is QUEUED — never dropped — while the view cannot yet receive it
		// (bridge not live yet, page still loading, osfui.onMessage not yet
		// installed, or the view hidden), and queued messages flush FIFO before
		// the view's first visible paint after a RequestMenu(viewId, true). So
		//     SendToWeb(v, ...); RequestMenu(v, true);
		// guarantees the page observes the message BEFORE it is on screen — open
		// a view directly in a target state with no default-face flash frames.
		// Queues are bounded (drop-OLDEST, warned in the OSF UI log), so a view
		// that never opens cannot grow memory unboundedly. Returns false only on
		// null arguments or a payload that won't parse. Detect the guarantee via
		// (GetInterfaceVersion() & 0xFFFF) >= 3; on MINOR <= 2 a send before the
		// bridge was ready returned false (dropped) instead.
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

		// ===== hotkey dispatch (MINOR 1.4 block, mcm-design.md §9) =====

		// --- hotkey subscription. Thread-safe; callbacks on the GAME MAIN
		// thread. ---
		// Fires when the physical key CURRENTLY bound to the key-typed setting
		// (a_modId, a_key) is pressed. The binding is whatever the user set —
		// OSF UI re-resolves on every rebind, so consumers never track VK
		// codes. Gated on OSF UI's input context: a press typed into an
		// overlay text field or during a rebind capture does NOT fire; key
		// repeats don't fire. Duplicate bindings across mods all fire —
		// conflicts are surfaced in the settings UI, never blocking. No replay
		// (a hotkey is an event, not state) and no schema flag: EVERY
		// key-typed setting dispatches; this subscription is the native
		// delivery opt-in (a web view opts in by subscribing via settings.get
		// — it then receives `ui.hotkey {mod, key}` pushes). Subscribing to an
		// unknown or non-key setting is legal but silent until such a setting
		// exists. Returns a token for UnsubscribeHotkey; 0 on null/empty
		// a_modId/a_key or null a_fn.
		virtual std::uint32_t SubscribeHotkey(const char* a_modId, const char* a_key,
		                                      HotkeyFn a_fn, void* a_user) = 0;
		virtual void          UnsubscribeHotkey(std::uint32_t a_token) = 0;

		// ===== runtime view registration (MINOR 1.5) =====

		// --- register a view your mod ships. Thread-safe; applied on the next
		// main tick. ---
		// Loads views/<a_viewId>/ (a folder your mod installs next to OSF UI's
		// built-in views, discovered from its manifest.json at OSF UI boot) and
		// registers it as an openable SURFACE — so it appears in the hub catalog
		// and responds to RequestMenu / the web `menu.open` command — WITHOUT the
		// user's config.json having to list it in `views`. Ship the folder, call
		// RegisterView once after fetching the bridge, then RequestMenu when you
		// want it on screen:
		//     bridge->RegisterView("myview");
		//     ...
		//     bridge->SendToWeb("myview", "mymod.state", "{...}");   // optional
		//     bridge->RequestMenu("myview", true);
		// All three calls may be issued back-to-back from any thread — they land
		// in call order on the same main tick (register, then the queued send,
		// then the open; the SendToWeb 1.3 delivery guarantee applies).
		//
		// Idempotent: an id that is already a registered surface (listed in the
		// user's config.views, or registered earlier) is left untouched — the
		// live view is NOT reloaded. Registering an id whose views/<id>/ folder
		// is missing warns in the OSF UI log and does nothing (install the view
		// folder with your mod). A view torn down by crash-recovery exhaustion
		// may be revived by calling RegisterView again (fresh retry budget).
		// The manifest's `openOnStart` is honored on registration.
		// Returns false only on a null/empty id; true = queued.
		virtual bool RegisterView(const char* a_viewId) = 0;

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
	//   if ((bridge->GetInterfaceVersion() & 0xFFFF) >= 4) { /* hotkeys ok */ }
	//   if ((bridge->GetInterfaceVersion() & 0xFFFF) >= 5) { /* RegisterView ok */ }
	// MINOR also gates behavioral guarantees: >= 3 means SendToWeb's
	// queue-until-deliverable + message-before-first-paint guarantee holds (so
	// e.g. a consumer can retire an "opening veil" that hid its UI until the
	// first state push landed).
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
