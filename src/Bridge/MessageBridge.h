#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <unordered_set>  // not in pch.h

#include <nlohmann/json.hpp>

// Protocol 2.0 bridge with typed send/request endpoints and a page-initiated ready/state/event handshake.

namespace OSFUI
{
	class MessageBridge
	{
	public:
		// Transport for native -> web text: deliver `a_json` to view `a_viewId`.
		using SendFn = std::function<void(std::string_view a_viewId, std::string_view a_json)>;

		// Send handlers are notifications and never settle a response.
		using SendHandler = std::function<void(const nlohmann::json& a_payload, MessageBridge& a_bridge)>;

		// Request handlers must Respond, Reject, or Defer exactly once.
		using RequestHandler = std::function<void(const nlohmann::json& a_payload, MessageBridge& a_bridge)>;
		enum class FallbackEndpointKind : std::uint8_t
		{
			kNone,
			kSend,
			kRequest,
		};
		using FallbackProbe = std::function<FallbackEndpointKind(std::string_view a_sourceViewId, std::string_view a_name)>;
		using FallbackHandler = std::function<void(std::string_view a_name, const nlohmann::json& a_payload, MessageBridge& a_bridge)>;

		// Runs after ready and before the event gate opens, preserving ready < state < events.
		using HelloHook = std::function<void(std::string_view a_viewId)>;

		// Only faults marked a_viewFault count toward view.protocol-misuse.
		using ProtocolFaultSink = std::function<void(std::string_view a_viewId, std::string_view a_code, std::string_view a_message, const nlohmann::json& a_detail, bool a_viewFault)>;
		// Adapter cleanup invoked only when the bridge drops a deferred request before RespondTo/RejectTo can settle it (deadline or view teardown).
		using DeferredDropHandler = std::function<void()>;

		explicit MessageBridge(SendFn a_send);

		// Register or replace an exact endpoint; send and request names are disjoint.
		void RegisterSend(std::string a_name, SendHandler a_handler);
		bool RegisterRequest(std::string a_name, RequestHandler a_handler);
		// Frozen 1.x command adapter; new endpoints use the strict registries.
		bool RegisterCommand(std::string a_name, SendHandler a_handler);

		// Missing endpoints are harmless during hot cleanup and resync.
		void UnregisterSend(std::string_view a_name);
		void UnregisterRequest(std::string_view a_name);
		void UnregisterCommand(std::string_view a_name);

		// Optional bounded registry consulted only after exact native endpoints miss.
		void SetEndpointFallback(FallbackProbe a_probe, FallbackHandler a_send, FallbackHandler a_request);

		// Reject malformed or non-whitelisted page input without making it fatal.
		void HandleWebMessage(std::string_view a_viewId, std::string_view a_json);

		// Answer the current request; calls outside an unsettled handler are ignored.
		void Respond(const nlohmann::json& a_payload);
		// Fail the current request with a stable machine code and human message.
		void Reject(std::string_view a_code, std::string_view a_message = {});
		// Returns a process-unique token for bounded deferred settlement, or "" outside an unsettled request.
		[[nodiscard]] std::string Defer(DeferredDropHandler a_onDropped = {});

		// Stale, expired, and duplicate deferred-settlement tokens are ignored.
		void RespondTo(std::string_view a_token, const nlohmann::json& a_payload);
		void RespondJsonTo(std::string_view a_token, std::string_view a_payloadJson);
		void RejectTo(std::string_view a_token, std::string_view a_code, std::string_view a_message = {});

		// Queue one-shot events until hello to preserve message-before-first-paint delivery.
		void Emit(std::string_view a_viewId, std::string_view a_name, const nlohmann::json& a_payload);
		void EmitJson(std::string_view a_viewId, std::string_view a_name, std::string_view a_payloadJson);
		void Emit(const std::unordered_set<std::string>& a_viewIds, std::string_view a_name, const nlohmann::json& a_payload);
		void EmitJson(const std::unordered_set<std::string>& a_viewIds, std::string_view a_name, std::string_view a_payloadJson);

		// Publish latest-wins state only after hello; pre-hello documents receive replay instead.
		void PublishState(std::string_view a_viewId, std::string_view a_mod, std::string_view a_key,
			const nlohmann::json& a_value);
		void PublishJsonState(std::string_view a_viewId, std::string_view a_mod, std::string_view a_key,
			std::string_view a_valueJson);
		void PublishState(const std::unordered_set<std::string>& a_viewIds, std::string_view a_mod,
			std::string_view a_key, const nlohmann::json& a_value);

		// Broadcast platform state and events to every greeted view.
		void PublishStateAll(std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value);
		void EmitAll(std::string_view a_name, const nlohmann::json& a_payload);

		// Create a closed event gate that queues until the document says hello.
		void OnViewCreated(std::string_view a_viewId);
		// Drop the gate, queued events, and deferred requests owned by the view.
		void OnViewDestroyed(std::string_view a_viewId);

		void SetHelloHook(HelloHook a_hook) { _onHello = std::move(a_hook); }
		void SetProtocolFaultSink(ProtocolFaultSink a_sink) { _protocolFaultSink = std::move(a_sink); }

		// Main thread: expire overdue deferred requests with no-response.
		void Tick(std::chrono::steady_clock::time_point a_now = std::chrono::steady_clock::now());

		// Source view of the in-flight message, or empty outside dispatch.
		[[nodiscard]] std::string_view CurrentSource() const { return _currentSource; }

		// Set a_viewFault only when the view caused the reported runtime fault.
		void ReportProtocolFault(std::string_view a_viewId, std::string_view a_code, std::string_view a_message,
			const nlohmann::json& a_detail = nlohmann::json::object(), bool a_viewFault = true);

	private:
		struct Pending
		{
			std::string                           view;
			std::string                           requestId;  // the PAGE's id, echoed on the wire
			std::string                           name;
			std::chrono::steady_clock::time_point deadline;
			DeferredDropHandler                   onDropped;
		};
		struct Gate
		{
			// State opens during replay; events open afterward so replay cannot overtake the backlog.
			bool                    greeted{ false };     // state may flow
			bool                    eventsOpen{ false };  // events may flow
			std::deque<std::string> queued;               // encoded event envelopes
		};

		[[nodiscard]] static std::string EncodeEvent(std::string_view a_name, std::string_view a_payloadJson);
		[[nodiscard]] static std::string EncodeState(std::string_view a_mod, std::string_view a_key, std::string_view a_valueJson);
		[[nodiscard]] static std::string EncodeReply(std::string_view a_requestId, std::string_view a_payloadJson);
		[[nodiscard]] static std::string EncodeError(std::string_view a_requestId, std::string_view a_code, std::string_view a_message);

		void HandleHello(std::string_view a_viewId);
		[[nodiscard]] bool DispatchSend(const std::string& a_name, const nlohmann::json& a_payload);
		void DispatchRequest(const std::string& a_name, const std::string& a_id, const nlohmann::json& a_payload);
		void SendReady(std::string_view a_viewId);
		void DeliverEvent(std::string_view a_viewId, const std::string& a_encoded, std::string_view a_name);
		// Fold settlement into the in-flight message's completion trace.
		void NoteTracedReply(std::string_view a_what);

		SendFn                                            _send;
		std::unordered_map<std::string, SendHandler>      _sends;
		std::unordered_map<std::string, RequestHandler>   _requests;
		std::unordered_map<std::string, SendHandler>      _commands;
		FallbackProbe                                      _fallbackProbe;
		FallbackHandler                                    _fallbackSend;
		FallbackHandler                                    _fallbackRequest;
		std::unordered_map<std::string, Gate>             _gates;    // view id -> event gate
		std::unordered_map<std::string, Pending>          _pending;  // runtime token -> deferred request
		std::uint64_t                                     _nextDeferToken{ 1 };
		HelloHook                                         _onHello;
		ProtocolFaultSink                                  _protocolFaultSink;

		std::string _currentSource;     // source view of the in-flight message (reply target)
		std::string _currentRequestId;  // correlation id of the in-flight request ("" = none)
		std::string _currentName;       // endpoint name of the in-flight message
		bool        _settled{ false };  // the in-flight request was answered, rejected or deferred
		bool        _inMessage{ false };  // inside HandleWebMessage dispatch (arms trace folding)
		bool        _sendDelivered{ false };  // one-way completions are trace detail; faults/requests stay debug
		std::string _trace;               // what went back while _inMessage

		std::unordered_set<std::string> _warnedUnknownEndpoints;  // warn-once-per-name log dedupe
	};
}
