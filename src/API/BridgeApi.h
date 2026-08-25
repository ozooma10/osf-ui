#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <unordered_set>

#include "OSFUI_API.h"
#include "OSFUI_Diagnostics.h"
#include "OSFUI_Settings.h"
#include "OSFUI_Views.h"

#include "API/HotkeySubscriptions.h"
#include "API/SettingsMirror.h"
#include "API/SettingsSubscriptions.h"

namespace OSFUI
{
	class MessageBridge;
}

namespace OSFUI::API
{
	// True for a valid, non-platform endpoint.
	[[nodiscard]] bool IsUnreservedEndpointName(std::string_view a_name);

	class BridgeApi final :
		public Settings::ISettings,
		public Views::IViews,
		public Diagnostics::IDiagnostics
	{
	public:
		[[nodiscard]] static BridgeApi& Get();

		void          GetPluginVersion(std::uint32_t& a_major, std::uint32_t& a_minor, std::uint32_t& a_patch);
		const char*   GetBridgeProtocolVersion();
		bool          IsBridgeReady();
		bool          IsReady() override { return IsBridgeReady(); }
		void          RegisterCommand(const char* a_name, CommandFn a_handler, void* a_user);
		void          UnregisterCommand(const char* a_name);
		void          RegisterSend(const char* a_name, Views::SendFn a_handler, void* a_user) override;
		void          UnregisterSend(const char* a_name) override;
		bool          RegisterRelativePointer(const char* a_viewId, Views::RelativePointerFn a_handler, void* a_user) override;
		void          UnregisterRelativePointer(const char* a_viewId) override;
		bool          RegisterViewOpenPreflight(const char* a_viewId, Views::ViewOpenPreflightFn a_handler, void* a_user) override;
		void          UnregisterViewOpenPreflight(const char* a_viewId) override;
		bool          RegisterViewLifecycle(const char* a_viewId, Views::ViewLifecycleFn a_handler, void* a_user) override;
		void          UnregisterViewLifecycle(const char* a_viewId) override;
		void          RegisterRequest(const char* a_name, Views::RequestFn a_handler, void* a_user) override;
		void          RegisterRequest(const char* a_name, RequestFn a_handler, void* a_user);
		void          UnregisterRequest(const char* a_name) override;
		bool          SendToWeb(const char* a_viewId, const char* a_type, const char* a_payloadJson) override;
		bool          SetViewState(const char* a_modId, const char* a_key, const char* a_payloadJson) override;
		void          SetReadyCallback(Views::ReadyFn a_callback, void* a_user) override;
		bool          RequestMenu(const char* a_viewId, bool a_open) override;
		std::uint32_t SubscribeSettings(const char* a_modId, Settings::SettingChangedFn a_fn, void* a_user) override;
		void          UnsubscribeSettings(std::uint32_t a_token) override;
		bool          GetSettingBool(const char* a_modId, const char* a_key, bool* a_out) override;
		bool          GetSettingInt(const char* a_modId, const char* a_key, std::int64_t* a_out) override;
		bool          GetSettingFloat(const char* a_modId, const char* a_key, double* a_out) override;
		std::uint32_t GetSettingString(const char* a_modId, const char* a_key, char* a_buf, std::uint32_t a_bufLen) override;
		bool          RegisterSettingsSchema(const char* a_schemaJson);
		void          UnregisterSettingsSchema(const char* a_modId);
		std::uint32_t SubscribeHotkey(const char* a_modId, const char* a_key, Settings::HotkeyFn a_fn, void* a_user) override;
		void          UnsubscribeHotkey(std::uint32_t a_token) override;
		bool          RegisterView(const char* a_viewId) override;
		bool          ReportIssue(const char* a_modId, const char* a_id, const char* a_code, std::uint32_t a_severity, const char* a_subject, const char* a_contextJson) override;
		bool          ClearIssue(const char* a_modId, const char* a_id) override;
		bool          ClearIssuesExcept(const char* a_modId, const char* a_keepIdsJson) override;
		struct ViewPresentationRequest
		{
			std::string                           view;
			bool                                  open{ true };
			std::chrono::steady_clock::time_point requestedAt;
		};
		std::vector<ViewPresentationRequest> TakeViewPresentationRequests();

		void SetViewCatalog(const std::vector<std::string>& a_viewIds);
		void SetViewInstantiated(std::string_view a_viewId, bool a_instantiated);

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

		struct ViewStateOp
		{
			std::string    mod;
			std::string    key;
			nlohmann::json value;
		};
		std::vector<ViewStateOp> TakeViewStateOps();

		struct SchemaOp
		{
			nlohmann::json schema;  // null for unregister
			std::string    modId;
		};

		struct PendingBatch
		{
			std::vector<ViewPresentationRequest> presentation;
			std::vector<ViewStateOp>              state;
			std::vector<SchemaOp>                 schemas;
			std::vector<std::string>              viewRegistrations;
		};
		[[nodiscard]] PendingBatch TakePendingBatch();

		void NoteUnsupportedApiCaller(std::string a_moduleName, std::uint32_t a_major, std::uint32_t a_minor);
		struct UnsupportedCaller
		{
			std::string module;
			std::uint32_t major{ 0 };
			std::uint32_t minor{ 0 };
		};
		static constexpr std::size_t kMaxUnsupportedCallers = 32;
		std::vector<UnsupportedCaller> TakeUnsupportedApiCallers();

		std::vector<std::string> TakeViewRegistrations();

		[[nodiscard]] SettingsMirror& Mirror() { return _mirror; }

		[[nodiscard]] SettingsSubscriptions& Subscriptions() { return _subscriptions; }

		[[nodiscard]] HotkeySubscriptions& Hotkeys() { return _hotkeys; }

		void SetBridgeAvailability(MessageBridge* a_bridge);
		void PumpMainThread();

		// Main-thread relative-pointer dispatch.
		[[nodiscard]] bool HasRelativePointer(std::string_view a_viewId);
		bool DispatchRelativePointer(std::string_view a_viewId, Views::RelativePointerPhase a_phase, float a_dx = 0.0f, float a_dy = 0.0f, float a_wheel = 0.0f);

		enum class ViewOpenPreflightResult
		{
			kNoHandler,
			kAllowed,
			kDenied,
		};
		// Pre-presentation dispatch.
		[[nodiscard]] ViewOpenPreflightResult RunViewOpenPreflight(std::string_view a_viewId);

		// Menu lifecycle dispatch.
		bool DispatchViewLifecycle(const std::string& a_viewId, Views::ViewLifecyclePhase a_phase);

	private:
		BridgeApi() = default;
		~BridgeApi() = default;
		BridgeApi(const BridgeApi&) = delete;
		BridgeApi& operator=(const BridgeApi&) = delete;

		struct Registration
		{
			Views::SendFn fn{ nullptr };
			void*     user{ nullptr };
		};
		struct RelativePointerRegistration
		{
			Views::RelativePointerFn fn{ nullptr };
			void*             user{ nullptr };
		};
		struct ViewOpenPreflightRegistration
		{
			Views::ViewOpenPreflightFn fn{ nullptr };
			void*               user{ nullptr };
		};
		struct ViewLifecycleRegistration
		{
			Views::ViewLifecycleFn fn{ nullptr };
			void*           user{ nullptr };
		};
		struct PendingSend
		{
			std::string view;
			std::string type;
			std::string payloadJson;
		};

		struct RequestRegistration
		{
			Views::RequestFn fn{ nullptr };
			RequestFn legacyFn{ nullptr };
			void* user{ nullptr };
		};
		struct InflightRequest
		{
			std::uint64_t token{ 0 };
			std::string view;
			std::string deferToken;  // MessageBridge::Defer()'s token, not the page's request id
			std::string name;
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
		void DropInflightRequest(std::uint64_t) noexcept;
		void DispatchRequest(const std::string&, const RequestRegistration&, const nlohmann::json&, MessageBridge&);
		enum Pending : std::uint32_t
		{
			kPendingPump = 1u << 0,
			kPendingPresentation = 1u << 1,
			kPendingState = 1u << 2,
			kPendingViewRegistrations = 1u << 3,
			kPendingHealth = 1u << 4,
			kPendingUnsupported = 1u << 5,
			kPendingSchemas = 1u << 6,
		};
		void MarkPending(std::uint32_t a_bits) noexcept
		{
			_pending.fetch_or(a_bits, std::memory_order_release);
		}
		std::mutex                                    _mutex;
		std::atomic<std::uint32_t>                    _pending{ 0 };
		SettingsMirror                                _mirror;            // own locking; never touched under _mutex
		SettingsSubscriptions                         _subscriptions;     // own locking; never touched under _mutex
		HotkeySubscriptions                           _hotkeys;           // own locking; never touched under _mutex
		std::unordered_map<std::string, Registration>        _commands;          // frozen RegisterCommand set
		std::unordered_map<std::string, Registration>        _sends;             // strict RegisterSend set
		std::unordered_map<std::string, RequestRegistration> _requests;          // desired request set
		std::unordered_map<std::string, RelativePointerRegistration> _relativePointers;  // exact view owner, first-wins
		std::unordered_map<std::string, ViewOpenPreflightRegistration> _viewOpenPreflights;  // exact view owner, first-wins
		std::unordered_map<std::string, ViewLifecycleRegistration> _viewLifecycles;  // exact view owner, first-wins
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
		std::vector<ViewStateOp>                      _pendingStateOps;    // SetViewState writes, drained by Runtime
		std::vector<SchemaOp>                         _pendingSchemaOps;   // deprecated schema registrations, drained by Runtime
		std::vector<UnsupportedCaller>                _unsupportedCallers;
		std::vector<std::string>                      _pendingViewRegs;    // RegisterView ids, drained by Runtime
		std::vector<HealthIssueOp>                    _pendingHealthIssueOps;
		MessageBridge*                                _bridge{ nullptr };         // non-owning; set on main thread
		MessageBridge*                                _appliedBridge{ nullptr };  // bridge we last applied to
		bool                                          _dirty{ false };            // endpoint set changed since apply
		Views::ReadyFn                                _readyCb{ nullptr };
		void*                                         _readyUser{ nullptr };
		std::condition_variable                       _readyInvokeCv;
		bool                                          _readyInvoking{ false };
		std::thread::id                               _readyInvokingThread{};
		bool                                          _readyFired{ false };
		std::atomic_bool                              _bridgeAvailable{ false };  // IsBridgeReady() compatibility fast path
	};
}
