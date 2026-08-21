// ============================================================================
// OSFUI_API.h - OSF UI native bridge API. Single copyable header; link nothing.
//
// Lets an SFSE plugin register bridge endpoints, publish data to a web view,
// and receive messages back - without compiling into OSFUI.dll. The public
// contract is encoded directly in this header.
//
// USE OSFUI::API::Client: it fetches the bridge, caches the native ABI version once,
// and turns a call the OSF UI runtime is too old for into a no-op.
//
// THREADING: status reads and typed getters are synchronous, callable from ANY thread.
// Mutating calls are thread-safe; their effect lands on the game main thread.
// All callbacks (SendFn/RequestFn/ReadyFn/SettingChangedFn/HotkeyFn/RelativePointerFn) run on the game main thread - keep them cheap.
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
	// ABI 1.x is append-only. Existing virtual slots never move; retired slots
	// remain inert tombstones. New capabilities append at the vtable tail and
	// bump MINOR. ABI
	// 1.8 added retained view state, 1.9 added strict one-way send endpoints,
	// and 1.10 adds view-owned relative-pointer capture.
	inline constexpr std::uint32_t kBridgeAPIVersion = (1u << 16) | 10u;
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
	// Frozen ABI 1.0 callback shape. RegisterCommand accepts both send() and
	// request(); requests receive an injected requestId and an automatic reply.
	// New endpoints should use RegisterSend or RegisterRequest instead.
	using CommandFn = SendFn;

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
	// Fired when the native bridge becomes available (a view is instantiated), and again after any re-creation. Main thread.
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

	// Main-thread phases for a view-owned relative-pointer session. kUpdate is
	// delivered at most once per game frame with all raw mouse packets since the
	// previous frame accumulated into dx/dy/wheel (wheel is DOM-style steps:
	// positive down/toward the user). kEnd is the ordinary LMB-up
	// edge; kCancel covers ownership loss, view teardown, or host recovery.
	enum class RelativePointerPhase : std::uint32_t
	{
		kBegin = 0,
		kUpdate = 1,
		kEnd = 2,
		kCancel = 3,
	};
	using RelativePointerFn = void (*)(const char* a_viewId,
	                                   RelativePointerPhase a_phase,
	                                   float a_dx,
	                                   float a_dy,
	                                   float a_wheel,
	                                   void* a_user) noexcept;

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

		// --- frozen command registration. Thread-safe; applied next main tick. ---
		// send() invokes the handler one-way. request() preserves the original
		// injected-requestId + automatic-reply contract. Prefer the strict tail
		// methods RegisterSend or RegisterRequest for new endpoints.
		//
		//   * Id: opaque non-empty string outside the reserved platform endpoints and osfui.* namespace.
		//     Register mod endpoints as "<modId>.<name>". A document owned by that
		//     mod may call the short <name>; cross-mod callers use the qualified id.
		//   * Duplicates: first-wins. To replace your OWN handler, UnregisterCommand then re-register (works within one tick).
		virtual void RegisterCommand(const char* a_name, CommandFn a_handler, void* a_user) = 0;
		virtual void UnregisterCommand(const char* a_name) = 0;

		// --- native -> web EVENTS. Thread-safe; queued to the target view. ---
		// Delivers { kind:"event", name: a_type, payload: <a_payloadJson> } to
		// a_viewId, where it arrives at osfui.on(a_type). When a_type is
		// "<ownMod>.<name>", that owning document may subscribe to short <name>.
		// a_payloadJson must be valid JSON. A known discovered-but-uninstantiated target retains a bounded
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
		// An input-capturing menu requested before OSF UI finishes its post-data-load
		// input integration remains pending and opens when that integration succeeds.
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

		// --- DEPRECATED settings registration. Thread-safe; merge lands next main tick. ---
		// Compatibility window for existing plugins only. New and updated mods MUST
		// ship settings/<modId>.json. These methods are planned for removal at the
		// next native ABI major; their frozen slots remain here throughout ABI 1.x.
		//
		// Returns false on malformed JSON or an invalid/missing id; true = queued.
		// A native schema retains its historical precedence over a drop-in with the
		// same id. User values use the same per-mod persistence file in either tier.
		[[deprecated("Ship settings/<modId>.json; runtime schema registration will be removed at the next native ABI major")]]
		virtual bool RegisterSettingsSchema(const char* a_schemaJson) = 0;
		[[deprecated("Remove the runtime registration call and ship settings/<modId>.json")]]
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
		// every current and future document of that mod receives it through the
		// local osfui.state.on("<a_key>") address; the qualified
		// "<a_modId>/<a_key>" address remains available. Latest wins per case-insensitive
		// key, at most 1024 keys per mod, and any JSON value is accepted.
		// Native state is not session-scoped; Papyrus state is because it may
		// contain form identities. Validation is synchronous.
		virtual bool SetViewState(const char* a_modId, const char* a_key, const char* a_payloadJson) = 0;

		// --- strict sends (ABI 1.9). Appended; every ABI 1.0-1.8 slot above is frozen. ---
		// A send endpoint is one-way: its payload is verbatim, a request naming it
		// is rejected wrong-endpoint-kind, and OSF UI never fabricates a reply.
		virtual void RegisterSend(const char* a_name, SendFn a_handler, void* a_user) = 0;
		virtual void UnregisterSend(const char* a_name) = 0;

		// --- relative pointer capture (ABI 1.10). Appended; every older slot is frozen. ---
		// Register one handler for an exact qualified view id. Registration is
		// first-wins and thread-safe; callbacks always run on the game main thread.
		// The page arms/disarms its session with the reserved
		// `osfui.relativePointer` send `{ "active": true|false }`. OSF UI accepts an
		// arm only from the currently visible input-owning menu, accumulates raw
		// WM_INPUT deltas without crossing the web/native pipe, and auto-cancels on
		// view ownership/lifecycle loss. Returns false for invalid/duplicate ids.
		virtual bool RegisterRelativePointer(const char* a_viewId, RelativePointerFn a_handler, void* a_user) = 0;
		virtual void UnregisterRelativePointer(const char* a_viewId) = 0;

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

	// Named features, valued as the additive ABI minor that introduced them.
	enum class Feature : std::uint32_t
	{
		kCommands = 0,
		kRequestMenu = 1,
		kSettings = 2,
		kDeliveryGuarantee = 3,
		kHotkeys = 4,
		kRegisterView = 5,
		kCommandShape = 6,
		kDiagnostics = 7,
		kRequests = 7,
		kViewState = 8,
		kSends = 9,
		kEndpointShape = 9,
		kRelativePointer = 10,
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

		// --- 1.0 frozen commands ---
		void RegisterCommand(const char* a_name, CommandFn a_handler, void* a_user) const noexcept
		{
			if (_bridge) {
				_bridge->RegisterCommand(a_name, a_handler, a_user);
			}
		}
		void UnregisterCommand(const char* a_name) const noexcept
		{
			if (_bridge) {
				_bridge->UnregisterCommand(a_name);
			}
		}

		// --- 1.9 strict sends ---
		void RegisterSend(const char* a_name, SendFn a_handler, void* a_user) const noexcept
		{
			if (Has(Feature::kSends)) {
				_bridge->RegisterSend(a_name, a_handler, a_user);
			}
		}
		void UnregisterSend(const char* a_name) const noexcept
		{
			if (Has(Feature::kSends)) {
				_bridge->UnregisterSend(a_name);
			}
		}

		// --- 1.10 view-owned relative pointer capture ---
		bool RegisterRelativePointer(const char* a_viewId, RelativePointerFn a_handler, void* a_user) const noexcept
		{
			return Has(Feature::kRelativePointer) && _bridge->RegisterRelativePointer(a_viewId, a_handler, a_user);
		}
		void UnregisterRelativePointer(const char* a_viewId) const noexcept
		{
			if (Has(Feature::kRelativePointer)) {
				_bridge->UnregisterRelativePointer(a_viewId);
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
		[[deprecated("Ship settings/<modId>.json; runtime schema registration will be removed at the next native ABI major")]]
		bool RegisterSettingsSchema(const char* a_schemaJson) const noexcept
		{
			return Has(Feature::kSettings) && _bridge->RegisterSettingsSchema(a_schemaJson);
		}
		[[deprecated("Remove the runtime registration call and ship settings/<modId>.json")]]
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
