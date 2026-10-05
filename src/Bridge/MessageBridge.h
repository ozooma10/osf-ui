#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_set>

#include <nlohmann/json.hpp>

// Protocol 2: typed send/request endpoints; page-initiated ready/state/event handshake.

namespace OSFUI
{
	class MessageBridge
	{
	public:
		// Native -> web text transport.
		using SendFn = std::function<void(std::string_view a_viewId, std::string_view a_json)>;

		// Send handlers do not reply.
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

		// Runs between ready and events, so state arrives before events.
		using HelloHook = std::function<void(std::string_view a_viewId)>;

		// Only faults marked a_viewFault count toward view.protocol-misuse.
		using ProtocolFaultSink = std::function<void(std::string_view a_viewId, std::string_view a_code, std::string_view a_message, const nlohmann::json& a_detail, bool a_viewFault)>;
		// Process-unique handle for a deferred request; 0 is never issued.
		using DeferToken = std::uint64_t;

		explicit MessageBridge(SendFn a_send);  // a_send must be callable; it is never null-checked

		// Replaces an existing endpoint of the same kind; send and request names are disjoint.
		void RegisterSend(std::string a_name, SendHandler a_handler);
		bool RegisterRequest(std::string a_name, RequestHandler a_handler);
		void RemoveEndpoint(const std::string& a_name);

		// Try the owning mod's name first, then the global name.
		void SetEndpointFallback(FallbackProbe a_probe, FallbackHandler a_send, FallbackHandler a_request);

		// Malformed or unknown page input is rejected, never fatal.
		void HandleWebMessage(std::string_view a_viewId, std::string_view a_json);

		// Ignored unless the current handler still owes a reply.
		void Respond(const nlohmann::json& a_payload);
		void Reject(std::string_view a_code, std::string_view a_message = {});
		// Returns 0 outside a pending request. Adapters keep only the token.
		[[nodiscard]] DeferToken Defer(std::chrono::milliseconds a_timeout = std::chrono::seconds(30));
		// Runtime thread.
		[[nodiscard]] bool HasPending(DeferToken a_token) const { return m_pending.contains(a_token); }
		[[nodiscard]] bool IsGreeted(std::string_view a_view) const
		{
			const auto found = m_gates.find(std::string(a_view));
			return found != m_gates.end() && found->second.greeted;
		}

		// Ignore stale, expired or duplicate reply tokens.
		void RespondTo(DeferToken a_token, const nlohmann::json& a_payload);
		void RespondJsonTo(DeferToken a_token, std::string_view a_payloadJson);
		void RejectTo(DeferToken a_token, std::string_view a_code, std::string_view a_message = {});
		// Settles every deferred request with the same error (e.g. game load).
		void RejectAll(std::string_view a_code, std::string_view a_message = {});

		// Queued until hello, so events sent before first paint still arrive.
		void Emit(std::string_view a_viewId, std::string_view a_name, const nlohmann::json& a_payload);
		void EmitJson(std::string_view a_viewId, std::string_view a_name, std::string_view a_payloadJson);
		void Emit(const std::unordered_set<std::string>& a_viewIds, std::string_view a_name, const nlohmann::json& a_payload);
		void EmitJson(const std::unordered_set<std::string>& a_viewIds, std::string_view a_name, std::string_view a_payloadJson);

		// Keep the latest value; pages receive it through replay after hello.
		void PublishState(std::string_view a_viewId, std::string_view a_mod, std::string_view a_key,
			const nlohmann::json& a_value);
		void PublishJsonState(std::string_view a_viewId, std::string_view a_mod, std::string_view a_key,
			std::string_view a_valueJson);
		void PublishState(const std::unordered_set<std::string>& a_viewIds, std::string_view a_mod,
			std::string_view a_key, const nlohmann::json& a_value);

		// Hold events until the document says hello.
		void OnViewCreated(std::string_view a_viewId);
		// Clear the view's connection, queued events and pending requests.
		void OnViewDestroyed(std::string_view a_viewId);

		void SetHelloHook(HelloHook a_hook) { m_onHello = std::move(a_hook); }
		void SetProtocolFaultSink(ProtocolFaultSink a_sink) { m_protocolFaultSink = std::move(a_sink); }

		// Runtime update: expires overdue deferred requests with no-response.
		void Tick(std::chrono::steady_clock::time_point a_now = std::chrono::steady_clock::now());

		// Source view of the in-flight message, or empty outside dispatch.
		[[nodiscard]] std::string_view CurrentSource() const { return m_currentSource; }

		void ReportProtocolFault(std::string_view a_viewId, std::string_view a_code, std::string_view a_message,
			const nlohmann::json& a_detail = nlohmann::json::object(), bool a_viewFault = true);

	private:
		struct Pending
		{
			std::string                           view;
			std::string                           requestId;  // the PAGE's id, echoed on the wire
			std::string                           name;
			std::chrono::steady_clock::time_point deadline;
		};
		struct Gate
		{
			// State opens during replay, events after, so replay cannot overtake the backlog.
			bool                    greeted{ false };     // state may flow
			bool                    eventsOpen{ false };  // events may flow
			std::deque<std::string> queued;               // encoded event envelopes
		};

		[[nodiscard]] static std::string EncodeEvent(std::string_view a_name, std::string_view a_payloadJson);
		[[nodiscard]] static std::string EncodeState(std::string_view a_mod, std::string_view a_key, std::string_view a_valueJson);
		[[nodiscard]] static std::string EncodeReply(std::string_view a_requestId, std::string_view a_payloadJson);
		[[nodiscard]] static std::string EncodeError(std::string_view a_requestId, std::string_view a_code, std::string_view a_message);

		void HandleHello(std::string_view a_viewId);
		struct ResolvedEndpoint
		{
			std::string name;
			FallbackEndpointKind kind{ FallbackEndpointKind::kNone };
			bool fallback{ false };
		};
		[[nodiscard]] ResolvedEndpoint ResolveEndpoint(const std::string& a_name) const;
		[[nodiscard]] bool DispatchSend(const std::string& a_name, const nlohmann::json& a_payload);
		void DispatchRequest(const std::string& a_name, const std::string& a_id, const nlohmann::json& a_payload);
		void SendReady(std::string_view a_viewId);
		void DeliverEvent(std::string_view a_viewId, const std::string& a_encoded, std::string_view a_name);
		void NoteTracedReply(std::string_view a_what);
		// No reply channel: logged, reported and traced.
		void RejectInvalid(std::string_view a_viewId, std::string_view a_message, const nlohmann::json& a_detail = {});
		void WarnUnknownEndpointOnce(const std::string& a_name, std::string_view a_what);
		// nullopt if already answered, expired, or the view is gone.
		[[nodiscard]] std::optional<Pending> TakePending(DeferToken a_token);

		SendFn                                            m_send;
		std::unordered_map<std::string, SendHandler>      m_sends;
		std::unordered_map<std::string, RequestHandler>   m_requests;
		FallbackProbe                                      m_fallbackProbe;
		FallbackHandler                                    m_fallbackSend;
		FallbackHandler                                    m_fallbackRequest;
		std::unordered_map<std::string, Gate>             m_gates;    // view id -> event gate
		std::unordered_map<DeferToken, Pending>           m_pending;  // runtime token -> deferred request
		HelloHook                                         m_onHello;
		ProtocolFaultSink                                  m_protocolFaultSink;

		std::string m_currentSource;     // source view of the in-flight message (reply target)
		std::string m_currentRequestId;  // correlation id of the in-flight request ("" = none)
		std::string m_currentName;       // endpoint name of the in-flight message
		bool        m_settled{ false };  // the in-flight request was answered, rejected or deferred
		bool        m_inMessage{ false };  // inside HandleWebMessage dispatch (arms trace folding)
		bool        m_sendDelivered{ false };  // one-way completions are trace detail; faults/requests stay debug
		std::string m_trace;               // what went back while m_inMessage

		std::unordered_set<std::string> m_warnedUnknownEndpoints;  // warn-once-per-name log dedupe
	};
}
