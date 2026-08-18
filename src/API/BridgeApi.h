#pragma once

#include <chrono>
#include <condition_variable>
#include <thread>
#include <unordered_set>

#include "OSFUI_API.h"  // IOSFUIBridge, callback types, version constants (sdk/, on the include path)

#include "API/HotkeySubscriptions.h"
#include "API/SettingsMirror.h"
#include "API/SettingsSubscriptions.h"

namespace OSFUI
{
	class MessageBridge;
}

namespace OSFUI::API
{
	// Concrete IOSFUIBridge singleton — the native-side implementation a sibling
	// SFSE plugin talks to via OSFUI_RequestBridge (src/API/Exports.cpp).
	//
	// All ABI methods are callable from any thread;
	// Send/request/bridge-availability callbacks fire on the main thread.
	class BridgeApi final : public IOSFUIBridge
	{
	public:
		[[nodiscard]] static BridgeApi& Get();

		// IOSFUIBridge ABI surface (any thread).
		std::uint32_t GetInterfaceVersion() override;
		void          GetPluginVersion(std::uint32_t& a_major, std::uint32_t& a_minor, std::uint32_t& a_patch) override;
		const char*   GetBridgeProtocolVersion() override;
		bool          IsBridgeReady() override;
		void          RegisterCommand(const char* a_name, CommandFn a_handler, void* a_user) override;
		void          UnregisterCommand(const char* a_name) override;
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
		// Runtime wiring (main thread only).
		// A menu open/close a sibling plugin requested via RequestMenu.
		struct ViewPresentationRequest
		{
			std::string view;
			bool        open{ true };
		};
		// Drain the queued menu requests. Runtime snapshots these at the top of
		// Tick and applies each through view-presentation policy (open/close +
		// ApplyViewPresentationPolicy) after PumpMainThread, which is what guarantees
		// SendToWeb lands before RequestMenu.
		std::vector<ViewPresentationRequest> TakeViewPresentationRequests();

		// Install the boot discovery catalog used by RequestMenu's synchronous
		// existence check, and mirror view instantiation/teardown transitions for close
		// validation. Runtime owns the catalog; these copies are protected by the
		// API mutex because RequestMenu is callable from any thread.
		void SetViewCatalog(const std::vector<std::string>& a_viewIds);
		void SetViewInstantiated(std::string_view a_viewId, bool a_instantiated);

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

		struct HealthIssueOp
		{
			enum class Kind { kReport, kClear, kClearExcept };
			Kind kind{ Kind::kReport };
			std::string modId;
			std::string id;
			std::string code;
			bool error{ false };
			std::string subject;
			nlohmann::json context;
			std::vector<std::string> keep;
		};
		std::vector<HealthIssueOp> TakeHealthIssueOps();

		// One queued SetViewState, already validated and parsed
		// synchronously; the store write happens on the main tick.
		struct ViewStateOp
		{
			std::string    mod;
			std::string    key;
			nlohmann::json value;
		};
		// Drain the queued state writes. Runtime retains each in the shared
		// RetainedStateStore (NOT session-scoped: unlike Papyrus values these hold
		// no form identities) and publishes it to the mod's instantiated views.
		std::vector<ViewStateOp> TakeViewStateOps();

		void NoteUnsupportedApiCaller(std::string a_moduleName, std::uint32_t a_major,
			std::uint32_t a_minor);
		struct UnsupportedCaller
		{
			std::string module;
			std::uint32_t major{ 0 };
			std::uint32_t minor{ 0 };
		};
		static constexpr std::size_t kMaxUnsupportedCallers = 32;
		std::vector<UnsupportedCaller> TakeUnsupportedApiCallers();

		// Drain queued RegisterView ids. Runtime validates each before the menu
		// request snapshot; openOnStart views are instantiated there, while ordinary
		// views stay uninstantiated. RegisterView -> SendToWeb -> RequestMenu issued back-to-back still
		// lands in one tick (ABI 1.5).
		std::vector<std::string> TakeViewRegistrations();

		// The any-thread settings value mirror the ABI typed getters read
		// Runtime::BuildModules feeds it from the store's
		// change/registry listeners on the main thread; the getters (and the
		// Papyrus natives) read it from any thread.
		[[nodiscard]] SettingsMirror& Mirror() { return _mirror; }

		// SubscribeSettings bookkeeping. The OSF UI runtime's store
		// change listener feeds OnChanged (right after Mirror().Update, main
		// thread); PumpMainThread drains replays + queued changes each tick.
		[[nodiscard]] SettingsSubscriptions& Subscriptions() { return _subscriptions; }

		// SubscribeHotkey bookkeeping. Runtime::DrainHotkeys
		// feeds OnFired (main thread); PumpMainThread drains the queue each tick.
		[[nodiscard]] HotkeySubscriptions& Hotkeys() { return _hotkeys; }

		// Hand the available MessageBridge (or nullptr when no bridge-enabled view exists)
		// to the API. A different pointer than last time triggers a full re-apply.
		void SetBridgeAvailability(MessageBridge* a_bridge);
		// Main thread; call each tick. (Re)applies the endpoint registry to the available
		// bridge, flushes queued sends, fires the compatibility availability callback once.
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
		std::unordered_map<std::string, Registration>        _commands;          // frozen RegisterCommand set
		std::unordered_map<std::string, Registration>        _sends;             // strict RegisterSend set
		std::unordered_map<std::string, RequestRegistration> _requests;          // desired request set
		std::vector<std::string>                      _pendingCommandUnregister;
		std::vector<std::string>                      _pendingSendUnregister;
		std::vector<std::string>                      _pendingRequestUnregister;
		std::unordered_map<std::uint64_t, InflightRequest> _inflightRequests;
		std::uint64_t                                 _nextRequestToken{ 1 };
		std::vector<PendingSend>                       _pendingSends;
		std::vector<ViewPresentationRequest>          _pendingViewPresentationRequests;  // RequestMenu compatibility ops, drained by Runtime
		std::unordered_set<std::string>               _knownViews;         // boot-discovered qualified view ids
		std::unordered_set<std::string>               _instantiatedViews;  // views with an instantiated document
		bool                                          _viewCatalogReady{ false };
		std::vector<SchemaOp>                         _pendingSchemaOps;   // schema (un)registrations, drained by Runtime
		std::vector<ViewStateOp>                      _pendingStateOps;    // SetViewState writes, drained by Runtime
		std::vector<UnsupportedCaller>                _unsupportedCallers;
		std::vector<std::string>                      _pendingViewRegs;    // RegisterView ids, drained by Runtime
		std::vector<HealthIssueOp>                    _pendingHealthIssueOps;
		MessageBridge*                                _bridge{ nullptr };         // non-owning; set on main thread
		MessageBridge*                                _appliedBridge{ nullptr };  // bridge we last applied to
		bool                                          _dirty{ false };            // endpoint set changed since apply
		ReadyFn                                       _readyCb{ nullptr };
		void*                                         _readyUser{ nullptr };
		std::condition_variable                       _readyInvokeCv;
		bool                                          _readyInvoking{ false };
		std::thread::id                               _readyInvokingThread{};
		bool                                          _readyFired{ false };
		std::atomic_bool                              _bridgeAvailable{ false };  // IsBridgeReady() compatibility fast path
	};
}
