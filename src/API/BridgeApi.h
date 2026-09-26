#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "OSFUI.h"
#include "Views/ViewRequestQueue.h"

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
		bool          RegisterViewLifecycle(const char* a_viewId, ViewLifecycleFn a_handler, void* a_user) noexcept override;
		void          UnregisterViewLifecycle(const char* a_viewId) noexcept override;
		bool          RegisterRequest(const char* a_name, RequestFn a_handler, void* a_user) noexcept override;
		bool          SendToWeb(const char* a_viewId, const char* a_type, const char* a_payloadJson) noexcept override;
		bool          SetViewState(const char* a_modId, const char* a_key, const char* a_payloadJson) noexcept override;
		void          SetReadyCallback(ReadyFn a_callback, void* a_user) noexcept override;
		bool          RequestMenu(const char* a_viewId, bool a_open) noexcept override;
		bool          RegisterView(const char* a_viewId) noexcept override;
		// Shared internal inbox for native, browser, and game-input requests.
		ViewRequestQueue& ViewRequests() { return m_viewRequests; }
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
			std::vector<ViewRequestQueue::Operation> presentation;
			std::vector<ViewStateOp>              state;
			std::vector<std::string>              viewRegistrations;
		};
		[[nodiscard]] PendingBatch TakePendingBatch();
		// Consume callback-published state without taking newly queued requests.
		[[nodiscard]] std::vector<ViewStateOp> TakePendingState();

		bool ClaimPapyrusEndpoint(std::string_view a_name);
		void ReleasePapyrusEndpoint(std::string_view a_name);
		void SetBridgeAvailability(MessageBridge* a_bridge);
		void PumpRuntimeCallbacks();

		// Runtime relative-pointer dispatch.
		[[nodiscard]] bool HasRelativePointer(std::string_view a_viewId);
		bool DispatchRelativePointer(std::string_view a_viewId, RelativePointerPhase a_phase, float a_dx = 0.0f, float a_dy = 0.0f, float a_wheel = 0.0f);

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
		// A plugin answer waiting for the main-thread pump; the bridge decides whether it is still live.
		struct QueuedReply
		{
			std::uint64_t token{ 0 };  // MessageBridge::Defer()'s token
			std::string payloadJson;
			bool        rejected{ false };
			std::string code;
			std::string message;
		};

		static void RespondThunk(std::uint64_t, const char*) noexcept;
		static void RejectThunk(std::uint64_t, const char*, const char*) noexcept;
		void RespondRequest(std::uint64_t, const char*) noexcept;
		void RejectRequest(std::uint64_t, const char*, const char*) noexcept;
		void QueueReply(QueuedReply) noexcept;
		void DispatchRequest(const std::string&, const RequestRegistration&, const nlohmann::json&, MessageBridge&);
		enum Pending : std::uint32_t
		{
			kPendingPump = 1u << 0,
			kPendingState = 1u << 1,
			kPendingViewRegistrations = 1u << 2,
		};
		void MarkPending(std::uint32_t a_bits) noexcept
		{
			m_pending.fetch_or(a_bits, std::memory_order_release);
		}
		std::unordered_set<std::string> m_papyrusEndpoints;
		std::mutex                                    m_mutex;
		// Unregister and SetReadyCallback wait for callbacks already dispatched on another thread.
		// Recursive so a callback may unregister or replace itself without deadlocking.
		std::recursive_mutex                          m_callbackDispatchMutex;
		std::atomic<std::uint32_t>                    m_pending{ 0 };
		std::unordered_map<std::string, Registration>        m_sends;             // strict RegisterSend set
		std::unordered_map<std::string, RequestRegistration> m_requests;          // desired request set
		std::unordered_map<std::string, RelativePointerRegistration> m_relativePointers;  // exact view owner, first-wins
		std::unordered_map<std::string, ViewLifecycleRegistration> m_viewLifecycles;  // exact view owner, first-wins
		std::vector<QueuedReply>                       m_queuedReplies;
		std::vector<PendingSend>                       m_pendingSends;
		ViewRequestQueue                              m_viewRequests;
		std::unordered_set<std::string>               m_knownViews;         // boot-discovered qualified view ids
		std::unordered_set<std::string>               m_instantiatedViews;  // views with an instantiated document
		bool                                          m_viewCatalogReady{ false };
		std::vector<ViewStateOp>                      m_pendingStateOps;    // SetViewState writes, drained by Runtime
		std::vector<std::string>                      m_pendingViewRegs;    // RegisterView ids, drained by Runtime
		MessageBridge*                                m_bridge{ nullptr };         // non-owning; set by Runtime
		MessageBridge*                                m_appliedBridge{ nullptr };  // bridge we last applied to
		bool                                          m_dirty{ false };            // endpoint set changed since apply
		ReadyFn                                m_readyCb{ nullptr };
		void*                                         m_readyUser{ nullptr };
		bool                                          m_readyFired{ false };
		std::atomic_bool                              m_bridgeAvailable{ false };
	};
}
