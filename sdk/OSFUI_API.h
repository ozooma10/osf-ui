// ============================================================================
// OSFUI_API.h - OSF UI native bridge API. Single copyable header; link nothing.
//
// Lets an SFSE plugin register bridge endpoints, publish data to a web view,
// and receive messages back - without compiling into OSFUI.dll. Full docs:
// docs/native-plugin-api.md.
//
// USE OSFUI::API::Client: it fetches the bridge, caches the native ABI version once,
// and turns a call the OSF UI runtime is too old for into a no-op.
//
// THREADING: status reads and typed getters are synchronous, callable from ANY thread.
// Mutating calls are thread-safe; their effect lands on the game main thread.
// All callbacks (SendFn/RequestFn/ReadyFn/SettingChangedFn/HotkeyFn) run on the game main thread - keep them cheap.
//
// LIFETIME: const char* params into callbacks are valid only for the call - copy what you retain.
// ============================================================================
#pragma once

#include <cstdint>
#include <type_traits>

// Loader dependency. In a CommonLibSF plugin the REX Win32 wrappers are used; a plain-Win32 consumer falls back to <Windows.h>.
// Define OSFUI_API_NO_REX to force the fallback even with REX on the include path.
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

static_assert(sizeof(void*) == 8, "OSFUI_API: x64 only (the vtable contract assumes the MSVC x64 ABI)");
static_assert(sizeof(std::uint32_t) == 4, "OSFUI_API: fixed-width ABI types required");

namespace OSFUI::API
{
	// Packed (MAJOR << 16) | MINOR.
	//
	// ABI 2.0 is the breaking native cleanup shipped with OSF UI 2.0. Future
	// additive methods append at the vtable tail and bump MINOR; another vtable
	// or behavioral break bumps MAJOR. During 2.0.x only, the export vends a
	// separate frozen 1.8 adapter to ABI 1.x callers; this 2.0 header remains
	// strict and that adapter is removed in 2.1.0.
	inline constexpr std::uint32_t kBridgeAPIVersion = (2u << 16) | 0u;
	inline constexpr std::uint32_t kBridgeAPIMajor   = kBridgeAPIVersion >> 16;
	inline constexpr std::uint32_t kBridgeAPIMinor   = kBridgeAPIVersion & 0xFFFFu;

	inline constexpr const wchar_t* kModuleName        = L"OSFUI.dll";
	inline constexpr const char*    kRequestExportName = "OSFUI_RequestBridge";

	// Handler for one registered send. Main thread. It is strictly one-way: a
	// request naming this endpoint is rejected `wrong-endpoint-kind`, no routing
	// fields are injected into the payload, and no acknowledgement is generated.
	//   a_name         : the registered endpoint name (one fn can serve many)
	//   a_payloadJson  : caller payload, verbatim JSON object
	//   a_sourceViewId : the sending view
	//   a_user         : the pointer you passed to RegisterSend
	using SendFn = void (*)(const char* a_name,
	                           const char* a_payloadJson,
	                           const char* a_sourceViewId,
	                           void*       a_user) noexcept;

	// Copyable, deferred reply token for a registered request.
	// Copies remain safe after response, timeout, or view closure: the opaque id
	// is resolved in OSF UI runtime-owned storage and a stale id is simply ignored.
	struct Request
	{
		using RespondFn = void (*)(std::uint64_t, const char*, const char*) noexcept;
		using RejectFn = void (*)(std::uint64_t, const char*, const char*) noexcept;

		// Compatibility field name: the registered request endpoint.
		const char* command{ nullptr };
		const char* payloadJson{ nullptr };
		const char* sourceViewId{ nullptr };

		void Respond(const char* a_payloadJson) const noexcept
		{
			if (_respond) _respond(_token, nullptr, a_payloadJson);
		}
		void Respond(const char* a_type, const char* a_payloadJson) const noexcept
		{
			if (_respond) _respond(_token, a_type, a_payloadJson);
		}
		void Reject(const char* a_code, const char* a_message = "") const noexcept
		{
			if (_reject) _reject(_token, a_code, a_message);
		}

		// OSF UI runtime-initialized ABI payload. Treat as opaque; copying it is supported.
		std::uint64_t _token{ 0 };
		RespondFn     _respond{ nullptr };
		RejectFn      _reject{ nullptr };
	};

	// Main-thread handler. The Request may be copied and answered once later
	// from any thread. Its const char* fields are valid only during this call.
	using RequestFn = void (*)(const Request& a_request, void* a_user) noexcept;
	static_assert(std::is_standard_layout_v<Request> && std::is_trivially_copyable_v<Request>);
	// Fired when the native bridge becomes available (a nativeBridge view is instantiated), and again after any re-creation. Main thread.
	using ReadyFn = void (*)(void* a_user) noexcept;

	// Fired for every committed value of a mod subscribed via SubscribeSettings.
	// a_valueJson is serialized JSON, e.g. "true", "1.5", "\"compact\"".
	// May deliver the same value twice around the subscribe window. Main thread.
	using SettingChangedFn = void (*)(const char* a_modId,
	                                  const char* a_key,
	                                  const char* a_valueJson,
	                                  void*       a_user) noexcept;

	// Fired when the physical key currently bound to a key-typed setting (subscribed via SubscribeHotkey) is pressed. Main thread.
	using HotkeyFn = void (*)(const char* a_modId,
	                          const char* a_key,
	                          void*       a_user) noexcept;

	enum class IssueSeverity : std::uint32_t
	{
		kWarning = 0,
		kError = 1,
	};

	struct IOSFUIBridge
	{
		// --- versioning / status. ANY thread, synchronous. ---
		virtual std::uint32_t GetInterfaceVersion() = 0;
		// Compatibility name: returns the installed OSF UI release version.
		virtual void          GetPluginVersion(std::uint32_t& a_major,
		                                       std::uint32_t& a_minor,
		                                       std::uint32_t& a_patch) = 0;
		// Web bridge protocol version, e.g. "2.0". Informational (log it);
		// gate on the ABI MINOR instead. Static string.
		virtual const char*   GetBridgeProtocolVersion() = 0;
		virtual bool          IsBridgeReady() = 0;  // compatibility name: at least one bridge-enabled document is instantiated

		// --- send registration. Thread-safe; applied next main tick. ---
		// Register a handler for an EXACT send endpoint. This endpoint is strictly
		// one-way; use RegisterRequest when the page needs an outcome.
		//
		//   * Id: "<author>.<modname>.<name>" - the mod id is lowercase [a-z0-9-] segments with dots
		//   * Duplicates: first-wins. To replace your OWN handler, UnregisterSend then re-register (works within one tick).
		virtual void RegisterSend(const char* a_name, SendFn a_handler, void* a_user) = 0;
		virtual void UnregisterSend(const char* a_name) = 0;

		// --- native -> web EVENTS. Thread-safe; queued to the target view. ---
		// Delivers { kind:"event", name: a_type, payload: <a_payloadJson> } to
		// a_viewId, where it arrives at osfui.on(a_type). a_payloadJson must be
		// valid JSON. A known discovered-but-uninstantiated target retains a bounded
		// FIFO until its page is created and greets the bridge.
		//
		// An event is a ONE-SHOT HAPPENING: delivered at most once, never
		// replayed. Data that is true until it changes — a status, a list, a
		// count — is STATE: publish it with SetViewState and the OSF UI runtime replays
		// it to every fresh document for you. Using an event for state is the
		// blank-after-F5 bug.
		//
		// Returns false only on null args or an unparseable payload.
		virtual bool SendToWeb(const char* a_viewId, const char* a_type, const char* a_payloadJson) = 0;

		// --- bridge-availability notification. Callback on the main thread. ---
		// SetReadyCallback is the frozen compatibility name.
		virtual void SetReadyCallback(ReadyFn a_callback, void* a_user) = 0;

		// --- view presentation. Thread-safe; applied next main tick. ---
		// Open/close a view by qualified "<modId>/<viewName>" id. RequestMenu is
		// the frozen ABI name and accepts both menu and HUD views.
		// Opening a valid folder under views/<modId>/<viewName>/ instantiates it on demand.
		//
		// Returns:
		//   * open  - true if a target exists and was queued; false if none was found.
		//   * close - true only for an already-instantiated view; never loads one.
		virtual bool RequestMenu(const char* a_viewId, bool a_open) = 0;

		// ===== settings consumption =====

		// --- change subscription. Thread-safe; callbacks on the main thread. ---
		// Fires for every committed value of a_modId. Per-mod, not per-key: switch on a_key inside your handler.
		//
		//   * Replayed once per current value on subscribe - and again if the mod registers later.
		//     DEFAULT (the user's saved value is kept on disk).
		//
		// Returns a token for UnsubscribeSettings; 0 on null a_modId/a_fn.
		virtual std::uint32_t SubscribeSettings(const char* a_modId, SettingChangedFn a_fn, void* a_user) = 0;
		virtual void          UnsubscribeSettings(std::uint32_t a_token) = 0;

		// --- typed getters. Synchronous, ANY thread. false / 0 on unknown mod/key or type mismatch. ---
		virtual bool GetSettingBool(const char* a_modId, const char* a_key, bool* a_out) = 0;
		virtual bool GetSettingInt(const char* a_modId, const char* a_key, std::int64_t* a_out) = 0;
		virtual bool GetSettingFloat(const char* a_modId, const char* a_key, double* a_out) = 0;
		// Covers string, enum, and key (key NAME, e.g. "F10").
		// Returns required length INCLUDING NUL (0 on unknown/mismatch);
		// copies up to a_bufLen bytes, always NUL-terminated when a_bufLen > 0.
		// Null/empty buffer = "how big?" probe. (type:"flags" values are arrays - no typed getter; read them from SettingChangedFn's JSON.)
		virtual std::uint32_t GetSettingString(const char* a_modId, const char* a_key, char* a_buf, std::uint32_t a_bufLen) = 0;

		// --- settings registration. Thread-safe; merge lands next main tick. ---
		// a_schemaJson is the same document a settings/<modId>.json drop-in would hold (id = "<author>.<modname>").
		//
		//   * Returns false on a parse/shape error (malformed JSON, non-object, missing/invalid "id"); true = queued.
		//   * User values overlay from the same per-mod file as the drop-in tier, so a mod can migrate tiers without losing settings.
		//   * Conflicts: this wins over a drop-in of the same id (warned); it replaces an earlier native registration of the same id.
		virtual bool RegisterSettingsSchema(const char* a_schemaJson) = 0;
		// Drops a native-registered schema (user's values file kept). Ignored (warned) for ids owned by drop-in files.
		virtual void UnregisterSettingsSchema(const char* a_modId) = 0;

		// ===== hotkey dispatch =====

		// --- hotkey subscription. Thread-safe; callbacks on the main thread. ---
		// Fires when the physical key CURRENTLY bound to the key-typed setting (a_modId, a_key) is pressed.
		// OSF UI re-resolves on rebind, so consumers never track VK codes.
		//
		//   * Gated by UI input policy: no fire while typing into a text field; key repeats don't fire.
		//   * Duplicate bindings across mods all fire.
		//
		// Returns a token; 0 on null/empty a_modId/a_key or null a_fn.
		virtual std::uint32_t SubscribeHotkey(const char* a_modId, const char* a_key,
		                                      HotkeyFn a_fn, void* a_user) = 0;
		virtual void          UnsubscribeHotkey(std::uint32_t a_token) = 0;

		// ===== runtime view registration =====

		// --- register a view your mod ships. Thread-safe; applied next main tick.
		// a_viewId is the qualified "<modId>/<viewName>" id of a views/<modId>/<viewName>/ folder your mod installs.
		// Validates it as an openable view (discovery already catalogs it).
		// Ordinary views remain uninstantiated until first open; manifest openOnStart
		// instantiates and opens immediately — RegisterView is plugin opt-in, so that applies
		// to menus too, unlike discovery-driven startup.
		//
		// Ship the folder, call once after fetching the bridge, then RequestMenu:
		//     bridge->RegisterView("acme.mymod/dashboard");
		//     bridge->SendToWeb("acme.mymod/dashboard", "acme.mymod.state", "{...}");  // optional
		//     bridge->RequestMenu("acme.mymod/dashboard", true);
		//
		//   * Idempotent: an already-instantiated view is left untouched (not reloaded).
		//   * A missing view folder warns and does nothing.
		//   * A view torn down by crash-recovery exhaustion gets a fresh retry budget on its next open.
		//   * Manifest `openOnStart` is honored on registration.
		//
		// Returns false only on a null/empty/invalid id; true = queued.
		virtual bool RegisterView(const char* a_viewId) = 0;

		// Publish and withdraw durable, local-only conditions shown in System
		// Health. These methods never upload data or open an external page.
		virtual bool ReportIssue(const char* a_modId, const char* a_id, const char* a_code,
			std::uint32_t a_severity, const char* a_subject, const char* a_contextJson) = 0;
		virtual bool ClearIssue(const char* a_modId, const char* a_id) = 0;
		virtual bool ClearIssuesExcept(const char* a_modId, const char* a_keepIdsJson) = 0;

		// Same name grammar and first-wins namespace as sends.
		virtual void RegisterRequest(const char* a_name, RequestFn a_handler, void* a_user) = 0;
		virtual void UnregisterRequest(const char* a_name) = 0;

		// --- native -> web retained MOD STATE. Thread-safe; applied next main tick.
		// SetViewState is the frozen ABI name. It publishes a_payloadJson as the
		// retained value of a_key for YOUR mod, not for one individual view;
		// every current and future document of that mod receives it through
		// osfui.state.on("<a_modId>/<a_key>"). Latest wins per case-insensitive
		// key, at most 64 keys per mod, and any JSON value is accepted.
		// Native state is not session-scoped; Papyrus state is because it may
		// contain form identities. Validation is synchronous.
		virtual bool SetViewState(const char* a_modId, const char* a_key, const char* a_payloadJson) = 0;

	protected:
		~IOSFUIBridge() = default;  // OSF UI owns the singleton; consumers never delete it.
	};

	using RequestBridge_t = IOSFUIBridge* (*)(std::uint32_t a_abiVersion) noexcept;

#ifdef _WIN32
	// Fetch ONCE and cache (or use Client below). Call after SFSE kPostLoad; do NOT call per-frame.
	// Returns nullptr if OSF UI is absent or its MAJOR differs from yours - a normal outcome; degrade (no UI) rather than fail.
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
	//     static OSFUI::API::Client g_ui;      // static/leaked: handlers may fire for process life
	//     if (g_ui.Init()) {                    // after SFSE kPostLoad
	//         g_ui.RegisterSend("acme.mymod.ping", &OnPing, nullptr);
	//         if (g_ui.Has(OSFUI::API::Feature::kRegisterView)) {
	//             g_ui.RegisterView("acme.mymod/dashboard");
	//         }
	//     }
	//
	// ========================================================================

	// Named feature gates for future additive 2.x minors. Every feature present
	// in this header is part of the ABI 2.0 baseline.
	enum class Feature : std::uint32_t
	{
		kSends = 0,
		kRequestMenu = 0,
		kSettings = 0,
		kDeliveryGuarantee = 0,
		kHotkeys = 0,
		kRegisterView = 0,
		kEndpointShape = 0,
		kDiagnostics = 0,
		kRequests = 0,
		kViewState = 0,
	};

	class Client
	{
	public:
#ifdef _WIN32
		// Fetch + cache the bridge and its version. Call ONCE after SFSE
		// kPostLoad. Returns false when OSF UI is absent or MAJOR-mismatched;
		// every other method then degrades to false/0/no-op.
		bool Init(std::uint32_t a_abiVersion = kBridgeAPIVersion) noexcept
		{
			return Attach(RequestBridge(a_abiVersion));
		}
#endif

		// Adopt an already-fetched bridge (advanced use / test doubles). nullptr
		// detaches. Returns IsConnected().
		bool Attach(IOSFUIBridge* a_bridge) noexcept
		{
			_bridge = a_bridge;
			_minor = _bridge ? (_bridge->GetInterfaceVersion() & 0xFFFFu) : 0u;
			return _bridge != nullptr;
		}

		[[nodiscard]] explicit operator bool() const noexcept { return _bridge != nullptr; }
		[[nodiscard]] bool     IsConnected() const noexcept { return _bridge != nullptr; }

		// Does the OSF UI runtime support this native ABI feature? (It may be older than this header.)
		[[nodiscard]] bool Has(Feature a_feature) const noexcept
		{
			return _bridge && _minor >= static_cast<std::uint32_t>(a_feature);
		}

		// The raw interface for advanced use. YOU own version-gating if you call
		// tail vmethods through it.
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

		// --- 2.0 baseline ---
		void RegisterSend(const char* a_name, SendFn a_handler, void* a_user) const noexcept
		{
			if (_bridge) {
				_bridge->RegisterSend(a_name, a_handler, a_user);
			}
		}
		void UnregisterSend(const char* a_name) const noexcept
		{
			if (_bridge) {
				_bridge->UnregisterSend(a_name);
			}
		}
		bool SendToWeb(const char* a_viewId, const char* a_type, const char* a_payloadJson) const noexcept
		{
			return _bridge && _bridge->SendToWeb(a_viewId, a_type, a_payloadJson);
		}
		bool SetViewState(const char* a_modId, const char* a_key, const char* a_payloadJson) const noexcept
		{
			return Has(Feature::kViewState) && _bridge->SetViewState(a_modId, a_key, a_payloadJson);
		}
		void SetReadyCallback(ReadyFn a_callback, void* a_user) const noexcept
		{
			if (_bridge) {
				_bridge->SetReadyCallback(a_callback, a_user);
			}
		}

		bool RequestMenu(const char* a_viewId, bool a_open) const noexcept
		{
			return Has(Feature::kRequestMenu) && _bridge->RequestMenu(a_viewId, a_open);
		}

		// --- settings ---
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

		// --- hotkeys ---
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

		// --- views ---
		bool RegisterView(const char* a_viewId) const noexcept
		{
			return Has(Feature::kRegisterView) && _bridge->RegisterView(a_viewId);
		}

		bool ReportIssue(const char* a_modId, const char* a_id, const char* a_code,
			IssueSeverity a_severity, const char* a_subject = "",
			const char* a_contextJson = nullptr) const noexcept
		{
			return Has(Feature::kDiagnostics) &&
				_bridge->ReportIssue(a_modId, a_id, a_code,
					static_cast<std::uint32_t>(a_severity), a_subject, a_contextJson);
		}
		bool ClearIssue(const char* a_modId, const char* a_id) const noexcept
		{
			return Has(Feature::kDiagnostics) && _bridge->ClearIssue(a_modId, a_id);
		}
		bool ClearIssuesExcept(const char* a_modId, const char* a_keepIdsJson) const noexcept
		{
			return Has(Feature::kDiagnostics) && _bridge->ClearIssuesExcept(a_modId, a_keepIdsJson);
		}

		// --- request/response ---
		void RegisterRequest(const char* a_name, RequestFn a_handler, void* a_user) const noexcept
		{
			if (Has(Feature::kRequests)) _bridge->RegisterRequest(a_name, a_handler, a_user);
		}
		void UnregisterRequest(const char* a_name) const noexcept
		{
			if (Has(Feature::kRequests)) _bridge->UnregisterRequest(a_name);
		}
	private:
		IOSFUIBridge* _bridge{ nullptr };
		std::uint32_t _minor{ 0 };
	};
}
