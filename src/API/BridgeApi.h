#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "OSFUI.h"

namespace OSFUI
{
	class MessageBridge;
}

namespace OSFUI::API
{
	// True for a valid, non-platform endpoint.
	[[nodiscard]] bool IsUnreservedEndpointName(std::string_view a_name);

	class BridgeApi final : public IUI
	{
	public:
		[[nodiscard]] static BridgeApi& Get();

		bool          IsReady() noexcept override;
		bool          RegisterSend(const char* a_name, SendFn a_handler, void* a_user) noexcept override;
		bool          RegisterRelativePointer(const char* a_viewId, RelativePointerFn a_handler, void* a_user) noexcept override;
		void          UnregisterRelativePointer(const char* a_viewId) noexcept override;
		bool          RegisterViewOpenPreflight(const char* a_viewId, ViewOpenPreflightFn a_handler, void* a_user) noexcept override;
		void          UnregisterViewOpenPreflight(const char* a_viewId) noexcept override;
		bool          RegisterViewLifecycle(const char* a_viewId, ViewLifecycleFn a_handler, void* a_user) noexcept override;
		void          UnregisterViewLifecycle(const char* a_viewId) noexcept override;
		bool          RegisterRequest(const char* a_name, RequestFn a_handler, void* a_user) noexcept override;
		bool          SendToWeb(const char* a_viewId, const char* a_type, const char* a_payloadJson) noexcept override;
		bool          SetViewState(const char* a_modId, const char* a_key, const char* a_payloadJson) noexcept override;
		void          SetReadyCallback(ReadyFn a_callback, void* a_user) noexcept override;
		bool          RequestMenu(const char* a_viewId, bool a_open) noexcept override;
		bool          RegisterView(const char* a_viewId) noexcept override;
		struct ViewPresentationRequest
		{
			std::string                           view;
			bool                                  open{ true };
			std::chrono::steady_clock::time_point requestedAt;
		};
		void SetViewCatalog(const std::vector<std::string>& a_viewIds);
		void SetViewInstantiated(std::string_view a_viewId, bool a_instantiated);

		struct ViewStateOp
		{
			std::string    mod;
			std::string    key;
			nlohmann::json value;
		};
		struct PendingBatch
		{
			std::vector<ViewPresentationRequest> presentation;
			std::vector<ViewStateOp>              state;
			std::vector<std::string>              viewRegistrations;
		};
		[[nodiscard]] PendingBatch TakePendingBatch();

		bool ClaimPapyrusEndpoint(std::string_view a_name);
		void ReleasePapyrusEndpoint(std::string_view a_name);
		void SetBridgeAvailability(MessageBridge* a_bridge);
		void PumpMainThread();

		// Main-thread relative-pointer dispatch.
		[[nodiscard]] bool HasRelativePointer(std::string_view a_viewId);
		bool DispatchRelativePointer(std::string_view a_viewId, RelativePointerPhase a_phase, float a_dx = 0.0f, float a_dy = 0.0f, float a_wheel = 0.0f);

		enum class ViewOpenPreflightResult
		{
			kNoHandler,
			kAllowed,
			kDenied,
		};
		// Pre-presentation dispatch.
		[[nodiscard]] ViewOpenPreflightResult RunViewOpenPreflight(std::string_view a_viewId);

		// Menu lifecycle dispatch.
		bool DispatchViewLifecycle(const std::string& a_viewId, ViewLifecyclePhase a_phase);

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
		struct RelativePointerRegistration
		{
			RelativePointerFn fn{ nullptr };
			void*             user{ nullptr };
		};
		struct ViewOpenPreflightRegistration
		{
			ViewOpenPreflightFn fn{ nullptr };
			void*               user{ nullptr };
		};
		struct ViewLifecycleRegistration
		{
			ViewLifecycleFn fn{ nullptr };
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
			RequestFn fn{ nullptr };
			void* user{ nullptr };
		};
		struct InflightRequest
		{
			std::string view;
			std::string deferToken;  // MessageBridge::Defer()'s token, not the page's request id
			bool answered{ false };
			bool rejected{ false };
			std::string payloadJson;
			std::string code;
			std::string message;
		};
		struct PendingReply
		{
			std::string deferToken;
			std::string payloadJson;
			bool        rejected{ false };
			std::string code;
			std::string message;
		};

		static void RespondThunk(std::uint64_t, const char*) noexcept;
		static void RejectThunk(std::uint64_t, const char*, const char*) noexcept;
		void RespondRequest(std::uint64_t, const char*) noexcept;
		void RejectRequest(std::uint64_t, const char*, const char*) noexcept;
		void DropInflightRequest(std::uint64_t) noexcept;
		void DispatchRequest(const std::string&, const RequestRegistration&, const nlohmann::json&, MessageBridge&);
		enum Pending : std::uint32_t
		{
			kPendingPump = 1u << 0,
			kPendingPresentation = 1u << 1,
			kPendingState = 1u << 2,
			kPendingViewRegistrations = 1u << 3,
		};
		void MarkPending(std::uint32_t a_bits) noexcept
		{
			m_pending.fetch_or(a_bits, std::memory_order_release);
		}
		std::unordered_set<std::string> m_papyrusEndpoints;
		std::mutex                                    m_mutex;
		// Unregister waits for callbacks already dispatched on another thread.
		// Recursive so a callback may unregister itself without deadlocking.
		std::recursive_mutex                          m_callbackDispatchMutex;
		std::atomic<std::uint32_t>                    m_pending{ 0 };
		std::unordered_map<std::string, Registration>        m_sends;             // strict RegisterSend set
		std::unordered_map<std::string, RequestRegistration> m_requests;          // desired request set
		std::unordered_map<std::string, RelativePointerRegistration> m_relativePointers;  // exact view owner, first-wins
		std::unordered_map<std::string, ViewOpenPreflightRegistration> m_viewOpenPreflights;  // exact view owner, first-wins
		std::unordered_map<std::string, ViewLifecycleRegistration> m_viewLifecycles;  // exact view owner, first-wins
		std::unordered_map<std::uint64_t, InflightRequest> m_inflightRequests;
		std::uint64_t                                 m_nextRequestToken{ 1 };
		std::vector<PendingSend>                       m_pendingSends;
		std::vector<ViewPresentationRequest>          m_pendingViewPresentationRequests;  // Drained by Runtime.
		std::unordered_set<std::string>               m_knownViews;         // boot-discovered qualified view ids
		std::unordered_set<std::string>               m_instantiatedViews;  // views with an instantiated document
		bool                                          m_viewCatalogReady{ false };
		std::vector<ViewStateOp>                      m_pendingStateOps;    // SetViewState writes, drained by Runtime
		std::vector<std::string>                      m_pendingViewRegs;    // RegisterView ids, drained by Runtime
		MessageBridge*                                m_bridge{ nullptr };         // non-owning; set on main thread
		MessageBridge*                                m_appliedBridge{ nullptr };  // bridge we last applied to
		bool                                          m_dirty{ false };            // endpoint set changed since apply
		ReadyFn                                m_readyCb{ nullptr };
		void*                                         m_readyUser{ nullptr };
		std::condition_variable                       m_readyInvokeCv;
		bool                                          m_readyInvoking{ false };
		std::thread::id                               m_readyInvokingThread{};
		bool                                          m_readyFired{ false };
		std::uint64_t m_readyRevision{ 0 };
		std::atomic_bool                              m_bridgeAvailable{ false };
	};
}
