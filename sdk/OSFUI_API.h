// ============================================================================
// OSFUI_API.h - OSF UI native bridge API.
// Copyable SINGLE header. Drop it into your SFSE plugin; link NOTHING.
//
// It lets a separate SFSE plugin register bridge commands, push data to a web
// view, and receive commands back from it, WITHOUT compiling its code into
// OSFUI.dll. See docs/native-plugin-api.md in the OSF UI repo.
//
// USE THE WRAPPER: OSFUI::API::Client (bottom of this header) is the primary,
// documented way to consume this API. It fetches the bridge, caches the
// host's version ONCE, and turns a call the host is too old for into a clean
// false/no-op instead of a jump past the end of the vtable. The raw
// IOSFUIBridge stays available for advanced use, but every example in the
// docs uses Client.
//
// PORTABILITY / ABI GROUND RULES (pre-committed - hold every future change
// to these):
//   * x64 only, MSVC object ABI (MSVC or clang-cl), C++17 minimum.
//   * The surface carries only primitives, UTF-8 const char*, function
//     pointers and void* user data - no STL, no nlohmann::json, no RE::*
//     types. It is therefore independent of the CommonLibSF pin.
//   * MAJOR breaks ABI; MINOR appends vmethods at the END of the vtable
//     only - never reordered, never removed, signatures never changed.
//   * Any future struct crossing this boundary leads with a
//     `std::uint32_t size` set by the caller (old host reads only what it
//     knows; new host honors the caller's smaller size).
//   * Any future enum/flag crossing the boundary is append-only, never
//     reordered, with 0 = unknown/none.
//   * MULTI-MAJOR POLICY: OSFUI_RequestBridge is a per-MAJOR dispatcher. A
//     future 2.0 host keeps vending the v1 interface to v1 callers - a MAJOR
//     bump obsoletes the old interface for NEW code, it does not break
//     shipped consumers.
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
// LIFETIME CONTRACTS:
//   * const char* parameters passed INTO your callbacks are valid only for
//     the duration of the call - copy anything you retain.
//   * A registered handler/callback may fire for the remaining PROCESS
//     lifetime (registrations survive bridge re-creation). Never point one
//     at an object you might free; prefer static/leaked state or
//     unregister first.
//   * The settings replay may deliver the same (mod, key, value) more than
//     once (subscribe-window overlap) - make SettingChangedFn idempotent.
//   * Strings returned BY the API (GetBridgeProtocolVersion) are static,
//     valid for process lifetime.
//
// VERSIONS AT A GLANCE: this C ABI (kBridgeAPIVersion, currently 1.x) is
//   the stable contract, even while the PLUGIN version and the WEB protocol
//   version (GetBridgeProtocolVersion, 0.x = unstable) are pre-1.0. Gate
//   native code on the ABI MINOR (Client does it for you); never parse the
//   protocol string - it belongs to the JS side's negotiation.
// ============================================================================
#pragma once

#include <cstdint>

// Loader dependency (Windows only — the interface itself is plain C++, so
// host-side unit tests compile this header on any platform): inside a
// CommonLibSF plugin the REX Win32 wrappers are used (no <Windows.h> macro
// leakage). A plain-Win32 consumer without CommonLibSF builds too - the
// fallback includes <Windows.h> (lean-and-mean) for GetModuleHandleW/
// GetProcAddress. Define OSFUI_API_NO_REX to force the fallback even when
// REX is on the include path.
#ifdef _WIN32
#	if !defined(OSFUI_API_NO_REX) && defined(__has_include)
#		if __has_include("REX/W32/KERNEL32.h")
#			define OSFUI_API_HAS_REX 1
#		endif
#	endif
#	ifdef OSFUI_API_HAS_REX
#		include "REX/W32/KERNEL32.h"  // GetModuleHandleW / GetProcAddress / HMODULE
#	else
#		ifndef WIN32_LEAN_AND_MEAN
#			define WIN32_LEAN_AND_MEAN
#		endif
#		ifndef NOMINMAX
#			define NOMINMAX
#		endif
#		include <Windows.h>
#	endif
#endif

static_assert(sizeof(void*) == 8, "OSFUI_API: x64 only (the host DLL is x64; the vtable contract assumes the MSVC x64 ABI)");
static_assert(sizeof(std::uint32_t) == 4, "OSFUI_API: fixed-width ABI types required");

namespace OSFUI::API
{
	// Packed (MAJOR << 16) | MINOR. MAJOR breaks ABI; MINOR bumps on an appended
	// vmethod — or on a strengthened behavioral guarantee a consumer may need
	// to detect (no vtable change; first done in 1.3).
	// History: 1.0 commands/sends/ready; 1.1 +RequestMenu; 1.2 +settings
	// (SubscribeSettings, typed getters, RegisterSettingsSchema); 1.3 SendToWeb
	// delivery guarantee (queue-until-deliverable + message-before-first-paint;
	// see SendToWeb below — no new vmethods); 1.4 +hotkeys (SubscribeHotkey/
	// UnsubscribeHotkey — every key-typed setting is a dispatchable binding);
	// 1.5 +RegisterView (load + surface-register a view folder your mod ships,
	// without the user's config.json listing it); 1.6 command-shape guarantee
	// (no vtable change): RegisterCommand accepts only
	// "<author>.<modname>.<name>" (two dots minimum — every platform command is
	// structurally unregisterable, replacing the old reserved-prefix list) and
	// REFUSES duplicates first-wins (replace your own handler explicitly:
	// UnregisterCommand, then re-register).
	inline constexpr std::uint32_t kBridgeAPIVersion = (1u << 16) | 6u;
	inline constexpr std::uint32_t kBridgeAPIMajor   = kBridgeAPIVersion >> 16;
	inline constexpr std::uint32_t kBridgeAPIMinor   = kBridgeAPIVersion & 0xFFFFu;

	inline constexpr const wchar_t* kModuleName        = L"OSFUI.dll";
	inline constexpr const char*    kRequestExportName = "OSFUI_RequestBridge";

	// Handler for one registered ui.command. Runs on the GAME (main) thread.
	// The const char* arguments are valid only for the duration of the call.
	//   a_command      : the command string registered (lets one fn serve many)
	//   a_payloadJson  : the command payload object, serialized - e.g. "{\"id\":\"x\"}".
	//                    Since web protocol 0.5 it may carry a "requestId"
	//                    field (the calling view's correlation id, injected by
	//                    the host). After your handler returns, the host acks
	//                    the caller with ui.result { ok:true } (= delivered);
	//                    publish richer results as your own message types via
	//                    SendToWeb, echoing the requestId in your payload if
	//                    you want correlation.
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
	// JSON text - e.g. "true", "1.5", "\"compact\"". May deliver the same
	// value twice around the subscribe window - be idempotent. The strings are
	// valid only for the duration of the call.
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
		// The native<->web protocol version, e.g. "0.5". INFORMATIONAL for
		// native code (log it for support) - gate on the ABI MINOR instead;
		// the JS side feature-detects via runtime.ready's capabilities list.
		// Static string, valid for process lifetime.
		virtual const char*   GetBridgeProtocolVersion() = 0;
		virtual bool          IsBridgeReady() = 0;             // a nativeBridge view is live

		// --- command registration. Thread-safe; applied on the next main tick. ---
		// Register the handler for an EXACT command string. Persists across
		// bridge re-creation.
		// SHAPE (MINOR >= 6, api-freeze-plan item 3): commands are
		// "<author>.<modname>.<name>" - your mod id (lowercase [a-z0-9-]
		// segments, exactly one dot) plus a name (which may itself contain
		// dots, e.g. "acme.mymod.catalog.get"). Anything else is refused with
		// a log warning: platform commands (dotless verbs, single-dot
		// "menu.open"/"game.get"/...) are structurally unregisterable, so
		// there is no reserved-prefix list to consult or drift.
		// DUPLICATES (MINOR >= 6): first-wins - registering an id someone
		// already owns is REFUSED (warned), not last-writer-wins. Replace
		// your OWN handler explicitly: UnregisterCommand, then re-register
		// (the pair works back-to-back within one tick). On MINOR <= 5 the
		// old reserved-prefix check and last-writer-wins apply instead.
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
		// null arguments or a payload that won't parse. On MINOR <= 2 a send
		// before the bridge was ready returned false (dropped) instead.
		virtual bool SendToWeb(const char* a_viewId, const char* a_type, const char* a_payloadJson) = 0;

		// --- readiness notification. Callback runs on the main thread. ---
		virtual void SetReadyCallback(ReadyFn a_callback, void* a_user) = 0;

		// --- menu control. Thread-safe; applied on the next main tick. ---
		// Open or close a registered SURFACE (menu/HUD view) by its qualified
		// "<modId>/<viewName>" id; the request is marshaled onto the main
		// thread and run through the normal menu policy. Lets a sibling plugin
		// surface its own view (e.g. an in-game item that opens the scene
		// browser). No-op if the id is not a registered surface.
		virtual bool RequestMenu(const char* a_viewId, bool a_open) = 0;

		// ===== settings consumption (MINOR 1.2 block, mcm-design.md §8.2) =====

		// --- change subscription. Thread-safe; callbacks on the GAME MAIN thread. ---
		// Fires for every committed value of a_modId, and is REPLAYED once per
		// current value on subscribe (and again if the mod registers later -
		// subscribing to a not-yet-registered id is legal; the replay arrives
		// when it registers). Per-mod, not per-key: one subscription, switch on
		// a_key. Returns a token for UnsubscribeSettings; 0 on null a_modId/a_fn.
		// FORWARD COMPAT (api-freeze-plan item 2): on a host that predates one
		// of the schema's setting TYPES, the replay/getters deliver the schema
		// DEFAULT for that setting (the user's saved value is preserved on
		// disk, served only once they upgrade). A schema whose `requires`
		// capabilities the host lacks is an inert stub: nothing replays and
		// getters return false.
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
		// (type:"flags" values are ARRAYS - no typed getter; consume them via
		// SettingChangedFn's JSON text.)
		virtual std::uint32_t GetSettingString(const char* a_modId, const char* a_key,
		                                       char* a_buf, std::uint32_t a_bufLen) = 0;

		// --- settings registration. Thread-safe; the merge lands on the next
		// main tick. ---
		// a_schemaJson is the SAME JSON document that would live in a
		// settings/<modId>.json drop-in (docs/schema in the OSF UI repo; the id
		// is "<author>.<modname>"). Returns false synchronously on a
		// parse/shape error: malformed JSON, a non-object document, or a
		// missing/grammar-violating "id" (deeper field problems fall back
		// defensively, exactly like a drop-in file). true = accepted and
		// queued. Persisted user values overlay from the same per-mod values
		// file as the drop-in tier, so a mod can migrate tiers without losing
		// settings. Same id as a drop-in file: this registration wins (warning
		// in the log); same id as an earlier runtime registration: replaced
		// (dev iteration). SubscribeSettings consumers of the id receive the
		// value replay when the merge commits.
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
		// a_viewId is the QUALIFIED "<modId>/<viewName>" id (api-freeze-plan
		// item 1) of a views/<modId>/<viewName>/ folder your mod installs next
		// to OSF UI's built-in views (discovered from its manifest.json at OSF
		// UI boot). Loads it and registers it as an openable SURFACE — it
		// appears in the views catalog (the Mods surface lists it, grouped on
		// your mod's settings page) and responds to RequestMenu / the web
		// `menu.open` command — WITHOUT the user's config.json having to list
		// it in `views`. Ship the folder, call RegisterView once after
		// fetching the bridge, then RequestMenu when you want it on screen:
		//     bridge->RegisterView("acme.mymod/dashboard");
		//     ...
		//     bridge->SendToWeb("acme.mymod/dashboard", "acme.mymod.state", "{...}");  // optional
		//     bridge->RequestMenu("acme.mymod/dashboard", true);
		// All three calls may be issued back-to-back from any thread — they land
		// in call order on the same main tick (register, then the queued send,
		// then the open; the SendToWeb 1.3 delivery guarantee applies).
		//
		// Idempotent: an id that is already a registered surface (listed in the
		// user's config.views, or registered earlier) is left untouched — the
		// live view is NOT reloaded. Registering an id whose view folder is
		// missing warns in the OSF UI log and does nothing (install the view
		// folder with your mod). A view torn down by crash-recovery exhaustion
		// may be revived by calling RegisterView again (fresh retry budget).
		// The manifest's `openOnStart` is honored on registration.
		// Returns false only on a null/empty/grammar-violating id; true = queued.
		virtual bool RegisterView(const char* a_viewId) = 0;

	protected:
		~IOSFUIBridge() = default;  // OSF UI owns the singleton; the consumer never deletes it.
	};

	using RequestBridge_t = IOSFUIBridge* (*)(std::uint32_t a_abiVersion) noexcept;

#ifdef _WIN32
	// FETCH ONCE and cache (or just use Client below, which does). Call after
	// SFSE kPostLoad. Do NOT call per-frame. Returns nullptr if OSF UI is
	// absent or its MAJOR differs from yours - a normal, expected outcome;
	// degrade (no UI) rather than failing. (Per the multi-major policy above,
	// a future 2.x host still vends the v1 interface to v1 callers.)
	inline IOSFUIBridge* RequestBridge(std::uint32_t a_abiVersion = kBridgeAPIVersion) noexcept
	{
#ifdef OSFUI_API_HAS_REX
		const REX::W32::HMODULE mod = REX::W32::GetModuleHandleW(kModuleName);
		if (!mod) {
			return nullptr;  // OSF UI not installed/loaded.
		}
		const auto fn = reinterpret_cast<RequestBridge_t>(
			REX::W32::GetProcAddress(mod, kRequestExportName));
#else
		const HMODULE mod = ::GetModuleHandleW(kModuleName);
		if (!mod) {
			return nullptr;  // OSF UI not installed/loaded.
		}
		const auto fn = reinterpret_cast<RequestBridge_t>(
			::GetProcAddress(mod, kRequestExportName));
#endif
		return fn ? fn(a_abiVersion) : nullptr;  // older OSF UI / MAJOR mismatch -> nullptr.
	}
#endif  // _WIN32

	// ========================================================================
	// Client - the version-gated wrapper (USE THIS).
	//
	// The raw bridge has a foot-gun: an OLDER 1.x OSFUI.dll hands you a bridge
	// whose vtable ends before the newest methods, and a call past that end is
	// UB — while the version check lives far away from the call site, where it
	// WILL be forgotten. Client caches the host MINOR once and gates every
	// method: on a too-old host the call returns false/0/no-op instead of
	// crashing, and Has(Feature) answers capability questions explicitly.
	//
	//     static OSFUI::API::Client g_ui;             // static/leaked: handlers
	//     ...                                          // may fire for process life
	//     if (g_ui.Init()) {                           // after SFSE kPostLoad
	//         g_ui.RegisterCommand("acme.mymod.ping", &OnPing, nullptr);
	//         if (g_ui.Has(OSFUI::API::Feature::kRegisterView)) {
	//             g_ui.RegisterView("acme.mymod/dashboard");
	//         }
	//     }
	//
	// Behavioral guarantees (delivery 1.3, command-shape 1.6) have no method
	// to gate — query them via Has() where your logic depends on them.
	// ========================================================================

	// Named features, valued as the ABI MINOR that introduced them.
	enum class Feature : std::uint32_t
	{
		kCommands = 0,           // RegisterCommand/SendToWeb/SetReadyCallback (any 1.x)
		kRequestMenu = 1,
		kSettings = 2,           // SubscribeSettings + typed getters + RegisterSettingsSchema
		kDeliveryGuarantee = 3,  // SendToWeb queue-until-deliverable + message-before-first-paint
		kHotkeys = 4,
		kRegisterView = 5,
		kCommandShape = 6,       // "<author>.<modname>.<name>" enforcement + first-wins duplicates
	};

	class Client
	{
	public:
#ifdef _WIN32
		// Fetch + cache the bridge and its version. Call ONCE after SFSE
		// kPostLoad. Returns false when OSF UI is absent or MAJOR-mismatched —
		// every other method then degrades to false/0/no-op, so "OSF UI not
		// installed" needs no special-casing at call sites.
		bool Init(std::uint32_t a_abiVersion = kBridgeAPIVersion) noexcept
		{
			return Attach(RequestBridge(a_abiVersion));
		}
#endif

		// Adopt an already-fetched bridge (advanced use / test doubles).
		// nullptr detaches. Returns IsConnected().
		bool Attach(IOSFUIBridge* a_bridge) noexcept
		{
			_bridge = a_bridge;
			_minor = _bridge ? (_bridge->GetInterfaceVersion() & 0xFFFFu) : 0u;
			return _bridge != nullptr;
		}

		[[nodiscard]] explicit operator bool() const noexcept { return _bridge != nullptr; }
		[[nodiscard]] bool     IsConnected() const noexcept { return _bridge != nullptr; }

		// Does the HOST support this feature? (The host may be older than
		// this header — that is the whole point of the gate.)
		[[nodiscard]] bool Has(Feature a_feature) const noexcept
		{
			return _bridge && _minor >= static_cast<std::uint32_t>(a_feature);
		}

		// The raw interface for advanced use. YOU own the version-gating if
		// you call tail vmethods through it.
		[[nodiscard]] IOSFUIBridge* Raw() const noexcept { return _bridge; }

		// --- status (0 / "" / false when not connected) ---
		[[nodiscard]] std::uint32_t GetInterfaceVersion() const noexcept
		{
			return _bridge ? _bridge->GetInterfaceVersion() : 0u;
		}
		void GetPluginVersion(std::uint32_t& a_major, std::uint32_t& a_minor, std::uint32_t& a_patch) const noexcept
		{
			if (_bridge) {
				_bridge->GetPluginVersion(a_major, a_minor, a_patch);
			} else {
				a_major = a_minor = a_patch = 0;
			}
		}
		[[nodiscard]] const char* GetBridgeProtocolVersion() const noexcept
		{
			return _bridge ? _bridge->GetBridgeProtocolVersion() : "";
		}
		[[nodiscard]] bool IsBridgeReady() const noexcept
		{
			return _bridge && _bridge->IsBridgeReady();
		}

		// --- 1.0 baseline ---
		void RegisterCommand(const char* a_command, CommandFn a_handler, void* a_user) const noexcept
		{
			if (_bridge) {
				_bridge->RegisterCommand(a_command, a_handler, a_user);
			}
		}
		void UnregisterCommand(const char* a_command) const noexcept
		{
			if (_bridge) {
				_bridge->UnregisterCommand(a_command);
			}
		}
		bool SendToWeb(const char* a_viewId, const char* a_type, const char* a_payloadJson) const noexcept
		{
			return _bridge && _bridge->SendToWeb(a_viewId, a_type, a_payloadJson);
		}
		void SetReadyCallback(ReadyFn a_callback, void* a_user) const noexcept
		{
			if (_bridge) {
				_bridge->SetReadyCallback(a_callback, a_user);
			}
		}

		// --- 1.1 ---
		bool RequestMenu(const char* a_viewId, bool a_open) const noexcept
		{
			return Has(Feature::kRequestMenu) && _bridge->RequestMenu(a_viewId, a_open);
		}

		// --- 1.2 settings ---
		std::uint32_t SubscribeSettings(const char* a_modId, SettingChangedFn a_fn, void* a_user) const noexcept
		{
			return Has(Feature::kSettings) ? _bridge->SubscribeSettings(a_modId, a_fn, a_user) : 0u;
		}
		void UnsubscribeSettings(std::uint32_t a_token) const noexcept
		{
			if (Has(Feature::kSettings)) {
				_bridge->UnsubscribeSettings(a_token);
			}
		}
		bool GetSettingBool(const char* a_modId, const char* a_key, bool* a_out) const noexcept
		{
			return Has(Feature::kSettings) && _bridge->GetSettingBool(a_modId, a_key, a_out);
		}
		bool GetSettingInt(const char* a_modId, const char* a_key, std::int64_t* a_out) const noexcept
		{
			return Has(Feature::kSettings) && _bridge->GetSettingInt(a_modId, a_key, a_out);
		}
		bool GetSettingFloat(const char* a_modId, const char* a_key, double* a_out) const noexcept
		{
			return Has(Feature::kSettings) && _bridge->GetSettingFloat(a_modId, a_key, a_out);
		}
		std::uint32_t GetSettingString(const char* a_modId, const char* a_key, char* a_buf, std::uint32_t a_bufLen) const noexcept
		{
			return Has(Feature::kSettings) ? _bridge->GetSettingString(a_modId, a_key, a_buf, a_bufLen) : 0u;
		}
		bool RegisterSettingsSchema(const char* a_schemaJson) const noexcept
		{
			return Has(Feature::kSettings) && _bridge->RegisterSettingsSchema(a_schemaJson);
		}
		void UnregisterSettingsSchema(const char* a_modId) const noexcept
		{
			if (Has(Feature::kSettings)) {
				_bridge->UnregisterSettingsSchema(a_modId);
			}
		}

		// --- 1.4 hotkeys ---
		std::uint32_t SubscribeHotkey(const char* a_modId, const char* a_key, HotkeyFn a_fn, void* a_user) const noexcept
		{
			return Has(Feature::kHotkeys) ? _bridge->SubscribeHotkey(a_modId, a_key, a_fn, a_user) : 0u;
		}
		void UnsubscribeHotkey(std::uint32_t a_token) const noexcept
		{
			if (Has(Feature::kHotkeys)) {
				_bridge->UnsubscribeHotkey(a_token);
			}
		}

		// --- 1.5 views ---
		bool RegisterView(const char* a_viewId) const noexcept
		{
			return Has(Feature::kRegisterView) && _bridge->RegisterView(a_viewId);
		}

	private:
		IOSFUIBridge* _bridge{ nullptr };
		std::uint32_t _minor{ 0 };
	};
}
