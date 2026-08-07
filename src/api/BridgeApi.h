#pragma once

#include <chrono>
#include <condition_variable>
#include <thread>
#include <unordered_set>

#include "OSFUI_API.h"  // IOSFUIBridge, SendFn, ReadyFn, version constants (sdk/, on the include path)

#include "api/HotkeySubscriptions.h"
#include "api/SettingsMirror.h"
#include "api/SettingsSubscriptions.h"

namespace OSFUI
{
	class MessageBridge;
}

namespace OSFUI::API
{
	// Concrete IOSFUIBridge singleton — the native-side implementation a sibling
	// SFSE plugin talks to via OSFUI_RequestBridge (src/api/Exports.cpp).
	//
	// All ABI methods are callable from any thread;
	// Send/request/ready callbacks fire on the main thread. See docs/native-plugin-api.md.
	class BridgeApi final : public IOSFUIBridge
	{
	public:
		[[nodiscard]] static BridgeApi& Get();

		// IOSFUIBridge ABI surface (any thread).
		std::uint32_t GetInterfaceVersion() override;
		void          GetPluginVersion(std::uint32_t& a_major, std::uint32_t& a_minor, std::uint32_t& a_patch) override;
		const char*   GetBridgeProtocolVersion() override;
		bool          IsBridgeReady() override;
		void          RegisterSend(const char* a_name, SendFn a_handler, void* a_user) override;
		void          UnregisterSend(const char* a_name) override;
		void          RegisterRequest(const char* a_name, RequestFn a_handler, void* a_user) override;
		void          UnregisterRequest(const char* a_name) override;
		bool          SendToWeb(const char* a_viewId, const char* a_type, const char* a_payloadJson) override;
		bool          SetViewState(const char* a_modId, const char* a_key, const char* a_payloadJson) override;
		void          SetReadyCallback(ReadyFn a_callback, void* a_user) override;
		bool          RequestMenu(const char* a_viewId, bool a_open) override;
		std::uint32_t SubscribeSettings(const char* a_modId, SettingChangedFn a_fn, void* a_user) override;
		void          UnsubscribeSettings(std::uint32_t a_token) override;
		bool          GetSettingBool(const char* a_modId, const char* a_key, bool* a_out) override;
		bool          GetSettingInt(const char* a_modId, const char* a_key, std::int64_t* a_out) override;
		bool          GetSettingFloat(const char* a_modId, const char* a_key, double* a_out) override;
		std::uint32_t GetSettingString(const char* a_modId, const char* a_key, char* a_buf, std::uint32_t a_bufLen) override;
		bool          RegisterSettingsSchema(const char* a_schemaJson) override;
		void          UnregisterSettingsSchema(const char* a_modId) override;
		std::uint32_t SubscribeHotkey(const char* a_modId, const char* a_key, HotkeyFn a_fn, void* a_user) override;
		void          UnsubscribeHotkey(std::uint32_t a_token) override;
		bool          RegisterView(const char* a_viewId) override;
		bool          ReportIssue(const char* a_modId, const char* a_id, const char* a_code,
					 std::uint32_t a_severity, const char* a_subject, const char* a_contextJson) override;
		bool          ClearIssue(const char* a_modId, const char* a_id) override;
		bool          ClearIssuesExcept(const char* a_modId, const char* a_keepIdsJson) override;

		// Temporary ABI 1.x endpoint kind. Kept out of IOSFUIBridge 2.0 so modern
		// consumers cannot opt back into request-id injection or auto-ack.
		void RegisterLegacyCommand(const char* a_name, SendFn a_handler, void* a_user);
		void UnregisterLegacyCommand(const char* a_name);
		bool RegisterLegacyRequest(const char* a_name, RequestFn a_handler, void* a_user);
		void UnregisterLegacyRequest(const char* a_name);

		// Runtime wiring (main thread only).
		// A menu open/close a sibling plugin requested via RequestMenu.
		struct MenuRequest
		{
			std::string view;
			bool        open{ true };
		};
		// Drain the queued menu requests. Runtime snapshots these at the top of
		// Tick and applies each through its own menu policy (_menus.Open/Close +
		// ApplyMenuPolicy) after PumpMainThread, which is what guarantees
		// SendToWeb lands before RequestMenu.
		std::vector<MenuRequest> TakeMenuRequests();

		// Install the boot discovery catalog used by RequestMenu's synchronous
		// existence check, and mirror surface load/unload transitions for close
		// validation. Runtime owns the catalog; these copies are protected by the
		// API mutex because RequestMenu is callable from any thread.
		void SetViewCatalog(const std::vector<std::string>& a_viewIds);
		void SetSurfaceLoaded(std::string_view a_viewId, bool a_loaded);

		// A queued RegisterSettingsSchema (schema is an object) or
		// UnregisterSettingsSchema (schema is null, modId set) — already
		// shape-validated synchronously; FIFO so register-then-unregister of
		// the same id lands in call order.
		struct SchemaOp
		{
			nlohmann::json schema;
			std::string    modId;
		};
		// Drain the queued schema ops. Runtime applies each to the SettingsStore
		// (Source::kNative) in DrainSchemaOps.
		std::vector<SchemaOp> TakeSchemaOps();

		// One queued health report from a sibling plugin (ABI 1.7). Already
		// validated synchronously; the registry write happens on the main tick.
		// FIFO across all three kinds so a report-then-sweep pair from one
		// producer lands in call order.
		struct DiagnosticOp
		{
			enum class Kind
			{
				kReport,
				kClear,
				kClearExcept,
			};
			Kind                     kind{ Kind::kReport };
			std::string              modId;    // the producing mod = the issue source
			std::string              id;       // kReport/kClear: producer-local issue id
			std::string              code;     // kReport only
			bool                     error{ false };  // kReport severity
			std::string              subject;  // kReport only
			nlohmann::json           context;  // kReport only (object; sanitized in the registry)
			std::vector<std::string> keep;     // kClearExcept only: producer-local ids to keep
		};
		// Drain the queued health reports. Runtime applies them to the
		// DiagnosticsModule in DrainDiagnosticOps, inside PumpDiagnostics, so the
		// broadcast that follows carries them.
		std::vector<DiagnosticOp> TakeDiagnosticOps();

		// One queued SetViewState, already validated and parsed
		// synchronously; the store write happens on the main tick.
		struct ViewStateOp
		{
			std::string    mod;
			std::string    key;
			nlohmann::json value;
		};
		// Drain the queued state writes. Runtime retains each in the shared
		// ViewStateStore (NOT session-scoped: unlike Papyrus values these hold
		// no form identities) and publishes it to the mod's live views.
		std::vector<ViewStateOp> TakeViewStateOps();

		// A plugin requested the bridge during SFSE load. ABI 1.x is temporarily
		// adapted; unrelated majors are refused. Record either outcome so Runtime
		// can raise one concrete, persistent compatibility card per DLL.
		void NoteLegacyApiCaller(std::string a_moduleName, std::uint32_t a_major,
			std::uint32_t a_minor, bool a_supported = false);
		struct LegacyCaller
		{
			std::string   module;  // bare DLL file name, "" when unresolvable
			std::uint32_t major{ 0 };
			std::uint32_t minor{ 0 };
			bool          supported{ false };
		};
		// One card per mod, bounded. The producer's own dedupe only covers the
		// window between drains — TakeLegacyApiCallers empties the set — so the
		// ACCUMULATING side has to re-apply both, or a plugin that retries on
		// every load screen grows the list for the whole session.
		static constexpr std::size_t kMaxLegacyCallers = 32;
		std::vector<LegacyCaller> TakeLegacyApiCallers();

		// Drain queued RegisterView ids. Runtime validates each before the menu
		// request snapshot; openOnStart views load there, while ordinary views stay
		// lazy. RegisterView -> SendToWeb -> RequestMenu issued back-to-back still
		// lands in one tick (ABI 1.5).
		std::vector<std::string> TakeViewRegistrations();

		// The any-thread settings value mirror the ABI typed getters read
		// (mcm-design.md §8.2). Runtime::BuildModules feeds it from the store's
		// change/registry listeners on the main thread; the getters (and the
		// Papyrus natives) read it from any thread.
		[[nodiscard]] SettingsMirror& Mirror() { return _mirror; }

		// SubscribeSettings bookkeeping (mcm-design.md §8.2). Runtime's store
		// change listener feeds OnChanged (right after Mirror().Update, main
		// thread); PumpMainThread drains replays + queued changes each tick.
		[[nodiscard]] SettingsSubscriptions& Subscriptions() { return _subscriptions; }

		// SubscribeHotkey bookkeeping (mcm-design.md §9). Runtime::DrainHotkeys
		// feeds OnFired (main thread); PumpMainThread drains the queue each tick.
		[[nodiscard]] HotkeySubscriptions& Hotkeys() { return _hotkeys; }

		// Hand the live MessageBridge (or nullptr when no nativeBridge view exists)
		// to the API. A different pointer than last time triggers a full re-apply.
		void OnBridgeReady(MessageBridge* a_bridge);
		// Main thread; call each tick. (Re)applies the endpoint registry to the live
		// bridge, flushes queued sends, fires the ready callback once.
		void PumpMainThread(std::chrono::steady_clock::time_point a_now = std::chrono::steady_clock::now());

	private:
		BridgeApi() = default;
		~BridgeApi() = default;
		BridgeApi(const BridgeApi&) = delete;
		BridgeApi& operator=(const BridgeApi&) = delete;

		struct Registration
		{
			SendFn fn{ nullptr };
			void*     user{ nullptr };
		};
		struct PendingSend
		{
			std::string view;
			std::string type;
			std::string payloadJson;
		};

		struct RequestRegistration
		{
			RequestFn fn{ nullptr };
			void*     user{ nullptr };
			bool      legacy{ false };
		};
		struct InflightRequest
		{
			std::uint64_t token{ 0 };
			std::string view;
			std::string deferToken;  // MessageBridge::Defer()'s token, not the page's request id
			std::string name;
			std::chrono::steady_clock::time_point deadline;
			bool answered{ false };
			bool rejected{ false };
			bool legacyReply{ false };
			std::string type;
			std::string payloadJson;
			std::string code;
			std::string message;
		};
		struct PendingReply
		{
			std::string view;
			std::string deferToken;
			std::string name;
			std::string payloadJson;
			std::string type;
			bool        rejected{ false };
			bool        legacyReply{ false };
			std::string code;
			std::string message;
		};

		static void RespondThunk(std::uint64_t, const char*, const char*) noexcept;
		static void RejectThunk(std::uint64_t, const char*, const char*) noexcept;
		void RespondRequest(std::uint64_t, const char*, const char*) noexcept;
		void RejectRequest(std::uint64_t, const char*, const char*) noexcept;
		void DispatchRequest(const std::string&, const RequestRegistration&,
			const nlohmann::json&, MessageBridge&);
		std::mutex                                    _mutex;
		SettingsMirror                                _mirror;            // own locking; never touched under _mutex
		SettingsSubscriptions                         _subscriptions;     // own locking; never touched under _mutex
		HotkeySubscriptions                           _hotkeys;           // own locking; never touched under _mutex
		std::unordered_map<std::string, Registration>        _sends;             // desired send set
		std::unordered_map<std::string, Registration>        _legacyCommands;    // temporary ABI 1.x command set
		std::unordered_map<std::string, RequestRegistration> _requests;          // desired request set
		std::unordered_map<std::string, RequestRegistration> _legacyRequests;    // temporary ABI 1.x request set
		std::vector<std::string>                      _pendingSendUnregister;
		std::vector<std::string>                      _pendingRequestUnregister;
		std::vector<std::string>                      _pendingLegacyCommandUnregister;
		std::vector<std::string>                      _pendingLegacyRequestUnregister;
		std::unordered_map<std::uint64_t, InflightRequest> _inflightRequests;
		std::uint64_t                                 _nextRequestToken{ 1 };
		std::vector<PendingSend>                       _pendingSends;
		std::vector<MenuRequest>                      _pendingMenuReqs;    // RequestMenu ops, drained by Runtime
		std::unordered_set<std::string>               _knownViews;         // boot-discovered manifest ids
		std::unordered_set<std::string>               _loadedViews;        // renderer surfaces with a live page
		bool                                          _viewCatalogReady{ false };
		std::vector<SchemaOp>                         _pendingSchemaOps;   // schema (un)registrations, drained by Runtime
		std::vector<ViewStateOp>                      _pendingStateOps;    // SetViewState writes, drained by Runtime
		std::vector<LegacyCaller>                     _legacyCallers;      // ABI-major-mismatched RequestBridge callers
		std::vector<std::string>                      _pendingViewRegs;    // RegisterView ids, drained by Runtime
		std::vector<DiagnosticOp>                     _pendingDiagnostics; // health reports, drained by Runtime
		MessageBridge*                                _bridge{ nullptr };         // non-owning; set on main thread
		MessageBridge*                                _appliedBridge{ nullptr };  // bridge we last applied to
		bool                                          _dirty{ false };            // endpoint set changed since apply
		ReadyFn                                       _readyCb{ nullptr };
		void*                                         _readyUser{ nullptr };
		std::condition_variable                       _readyInvokeCv;
		bool                                          _readyInvoking{ false };
		std::thread::id                               _readyInvokingThread{};
		bool                                          _readyFired{ false };
		std::atomic_bool                              _ready{ false };            // IsBridgeReady() fast path
	};
}
