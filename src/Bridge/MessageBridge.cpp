#include "Bridge/MessageBridge.h"

#include <atomic>

#include "Core/StringUtil.h"
#include "Core/Version.h"
#include "Core/Ids.h"
#include "Core/Json.h"

namespace OSFUI
{
	namespace
	{
		// Reject invalid caller-supplied correlation ids rather than silently demoting requests.
		constexpr std::size_t kMaxRequestIdLength = 64;

		// Bound view-supplied endpoint names on a UTF-8 codepoint boundary before encoding.
		constexpr std::size_t kMaxEndpointNameLength = 128;

		// Bound pre-hello events per view and drop the oldest on overflow.
		constexpr std::size_t kMaxQueuedEventsPerView = 64;

		// Bound concurrent deferred requests owned by one view.
		constexpr std::size_t kMaxPendingRequestsPerView = 64;

		// Process-wide so a late answer can never settle a request on a recreated bridge.
		std::atomic<std::uint64_t> g_nextDeferToken{ 1 };

		// Keep the runtime deadline longer than the page timer so timeout and no-response stay distinct.
		constexpr auto kRequestDeadline = std::chrono::seconds(30);

		[[nodiscard]] std::string BoundedEcho(std::string_view a_s, std::size_t a_limit = kMaxEndpointNameLength)
		{
			return std::string{ a_s.substr(0, StringUtil::Utf8TruncateLen(a_s, a_limit)) };
		}

		[[nodiscard]] std::string OwnerQualifiedEndpoint(std::string_view a_sourceViewId, std::string_view a_name)
		{
			const auto mod = Ids::ModOf(a_sourceViewId);
			if (mod.empty()) return {};
			return std::format("{}.{}", mod, a_name);
		}

		class ScopeExit
		{
		public:
			explicit ScopeExit(std::function<void()> a_fn) : m_fn(std::move(a_fn)) {}
			~ScopeExit() { m_fn(); }

			ScopeExit(const ScopeExit&) = delete;
			ScopeExit& operator=(const ScopeExit&) = delete;

		private:
			std::function<void()> m_fn;
		};

		// JSON string literal for a value we control the bounds of.
		[[nodiscard]] std::string Quote(std::string_view a_s)
		{
			return Json::Dump(nlohmann::json(std::string(a_s)));
		}

		// Trace only low-volume unsolicited pushes; settlements fold into their inbound trace.
		bool IsTracedState(std::string_view a_key)
		{
			return a_key == "views";
		}
	}

	MessageBridge::MessageBridge(SendFn a_send) :
		m_send(std::move(a_send))
	{}

	void MessageBridge::RegisterSend(std::string a_name, SendHandler a_handler)
	{
		if (m_requests.contains(a_name)) {
			REX::WARN("MessageBridge: [content] refused send endpoint '{}' — name already registered", a_name);
			return;
		}
		m_sends[std::move(a_name)] = std::move(a_handler);
	}

	bool MessageBridge::RegisterRequest(std::string a_name, RequestHandler a_handler)
	{
		if (m_sends.contains(a_name)) {
			REX::WARN("MessageBridge: [content] refused request endpoint '{}' — name already registered", a_name);
			return false;
		}
		// BridgeApi owns public first-wins policy; this internal trampoline is replaceable.
		m_requests[std::move(a_name)] = std::move(a_handler);
		return true;
	}

	void MessageBridge::SetEndpointFallback(FallbackProbe a_probe, FallbackHandler a_send, FallbackHandler a_request)
	{
		m_fallbackProbe = std::move(a_probe);
		m_fallbackSend = std::move(a_send);
		m_fallbackRequest = std::move(a_request);
	}

	void MessageBridge::HandleWebMessage(std::string_view a_viewId, std::string_view a_json)
	{
		// Remember the source so settlements route back to it.
		m_currentSource = std::string(a_viewId);
		m_currentRequestId.clear();
		m_currentName.clear();
		m_settled = false;
		m_inMessage = true;
		m_sendDelivered = false;
		m_trace.clear();

		// Restore the in-flight context on exit so a later Respond/Reject cannot settle an abandoned request.
		const ScopeExit cleanup([this] {
			m_inMessage = false;
			const auto result = m_trace.empty() ? std::string_view{ "(nothing)" } : std::string_view{ m_trace };
			if (m_sendDelivered) {
				REX::TRACE("MessageBridge: '{}' from view '{}' -> {}", m_currentName, m_currentSource, result);
			} else {
				REX::DEBUG("MessageBridge: '{}' from view '{}' -> {}", m_currentName, m_currentSource, result);
			}
			m_currentSource.clear();
			m_currentRequestId.clear();
			m_currentName.clear();
			m_settled = false;
			m_sendDelivered = false;
			m_trace.clear();
		});

		const auto msg = Json::Parse(a_json);
		if (!msg || !msg->is_object()) {
			// Unparseable input has no correlation id, so report through logs and the view console.
			ReportProtocolFault(a_viewId, "invalid-request", "malformed message", {});
			NoteTracedReply("invalid-request");
			return;
		}

		const auto kindIt = msg->find("kind");
		if (kindIt == msg->end() || !kindIt->is_string()) {
			ReportProtocolFault(a_viewId, "invalid-request", "kind is required and must be a string", {});
			NoteTracedReply("invalid-request");
			return;
		}
		const auto& kind = kindIt->get_ref<const std::string&>();
		const auto nameIt = msg->find("name");
		if (nameIt == msg->end() || !nameIt->is_string()) {
			ReportProtocolFault(a_viewId, "invalid-request", "name is required and must be a string", { { "kind", BoundedEcho(kind) } });
			NoteTracedReply("invalid-request");
			return;
		}
		const auto& rawName = nameIt->get_ref<const std::string&>();
		const auto name = BoundedEcho(rawName);
		m_currentName = name;

		if (kind != "send" && kind != "request") {
			ReportProtocolFault(a_viewId, "invalid-request", "kind must be \"send\" or \"request\"", { { "kind", BoundedEcho(kind) }, { "name", name } });
			NoteTracedReply("invalid-request");
			return;
		}
		if (rawName.empty() || rawName.size() > kMaxEndpointNameLength) {
			ReportProtocolFault(a_viewId, "invalid-request",
				std::format("name is required and must be at most {} bytes", kMaxEndpointNameLength), { { "kind", kind } });
			NoteTracedReply("invalid-request");
			return;
		}

		// Routing metadata sits beside the payload, so a payload field can never override it. The wire contract requires an object even when it is empty;
		const auto payloadIt = msg->find("payload");
		if (payloadIt == msg->end() || !payloadIt->is_object()) {
			ReportProtocolFault(a_viewId, "invalid-request", "payload is required and must be an object", { { "kind", kind }, { "name", name } });
			NoteTracedReply("invalid-request");
			return;
		}
		const auto& payload = *payloadIt;

		const auto idIt = msg->find("id");

		if (kind == "send") {
			// A send cannot carry an id because it never settles.
			if (idIt != msg->end()) {
				ReportProtocolFault(a_viewId, "invalid-request", "send messages carry no id — use a request",
					{ { "name", name } });
				NoteTracedReply("invalid-request");
				return;
			}
			m_sendDelivered = DispatchSend(name, payload);
			return;
		}

		if (idIt == msg->end() || !idIt->is_string()) {
			ReportProtocolFault(a_viewId, "invalid-request", "requests carry a string id",
				{ { "name", name } });
			NoteTracedReply("invalid-request");
			return;
		}
		const auto& id = idIt->get_ref<const std::string&>();
		if (id.empty() || id.size() > kMaxRequestIdLength) {
			ReportProtocolFault(a_viewId, "invalid-request",
				std::format("request id must be 1-{} characters", kMaxRequestIdLength),
				{ { "name", name } });
			NoteTracedReply("invalid-request");
			return;
		}
		DispatchRequest(name, id, payload);
	}

	MessageBridge::ResolvedEndpoint MessageBridge::ResolveEndpoint(const std::string& a_name) const
	{
		for (const auto& candidate : { OwnerQualifiedEndpoint(m_currentSource, a_name), a_name }) {
			if (candidate.empty()) continue;
			if (m_sends.contains(candidate)) return { candidate, FallbackEndpointKind::kSend, false };
			if (m_requests.contains(candidate)) return { candidate, FallbackEndpointKind::kRequest, false };
			if (m_fallbackProbe) {
				const auto kind = m_fallbackProbe(m_currentSource, candidate);
				if (kind != FallbackEndpointKind::kNone) return { candidate, kind, true };
			}
		}
		return {};
	}

	bool MessageBridge::DispatchSend(const std::string& a_name, const nlohmann::json& a_payload)
	{
		if (a_name == "osfui.hello") {
			HandleHello(m_currentSource);
			NoteTracedReply("ready+state");
			return true;
		}
		const auto endpoint = ResolveEndpoint(a_name);
		if (endpoint.kind == FallbackEndpointKind::kRequest) {
			ReportProtocolFault(m_currentSource, "wrong-endpoint-kind", "use request() for this endpoint", { { "name", a_name } });
			NoteTracedReply("wrong-endpoint-kind");
			return false;
		}
		if (endpoint.kind == FallbackEndpointKind::kSend) {
			if (endpoint.fallback) {
				if (m_fallbackSend) m_fallbackSend(endpoint.name, a_payload, *this);
			} else m_sends.at(endpoint.name)(a_payload, *this);
			return true;
		}
		constexpr std::size_t kMaxWarnedEndpoints = 512;
		if (m_warnedUnknownEndpoints.size() < kMaxWarnedEndpoints && m_warnedUnknownEndpoints.insert(a_name).second) {
			REX::WARN("MessageBridge: [content] dropped send to unknown endpoint '{}' (further drops of this endpoint are not logged)", a_name);
		}
		ReportProtocolFault(m_currentSource, "unknown-endpoint", "no such endpoint", { { "name", a_name } });
		NoteTracedReply("unknown-endpoint");
		return false;
	}

	void MessageBridge::DispatchRequest(const std::string& a_name, const std::string& a_id, const nlohmann::json& a_payload)
	{
		m_currentRequestId = a_id;
		const auto endpoint = ResolveEndpoint(a_name);
		if (endpoint.kind == FallbackEndpointKind::kSend) {
			Reject("wrong-endpoint-kind", "use send() for this endpoint");
			return;
		}
		if (endpoint.kind == FallbackEndpointKind::kNone) {
			constexpr std::size_t kMaxWarnedEndpoints = 512;
			if (m_warnedUnknownEndpoints.size() < kMaxWarnedEndpoints &&
				m_warnedUnknownEndpoints.insert(a_name).second) {
				REX::WARN("MessageBridge: [content] rejected request to unknown endpoint '{}' (further rejections of this endpoint are not logged)", a_name);
			}
			Reject("unknown-endpoint", "no such endpoint");
			return;
		}
		const auto owned = std::ranges::count_if(m_pending, [&](const auto& item) {
			return item.second.view == m_currentSource;
		});
		if (owned >= kMaxPendingRequestsPerView) {
			REX::WARN("MessageBridge: [content] view '{}' has {} requests in flight — refusing '{}'", m_currentSource, owned, a_name);
			Reject("request-capacity", "too many requests are already in flight for this view");
			return;
		}
		if (endpoint.fallback) {
			if (m_fallbackRequest) m_fallbackRequest(endpoint.name, a_payload, *this);
		} else m_requests.at(endpoint.name)(a_payload, *this);
		if (!m_settled) {
			REX::ERROR("MessageBridge: request endpoint '{}' returned without settling", a_name);
			Reject("internal", "the endpoint did not answer");
		}
	}

	void MessageBridge::HandleHello(std::string_view a_viewId)
	{
		// Preserve pre-hello events here; only OnViewCreated discards an old queue.
		m_gates[std::string(a_viewId)].greeted = true;

		// 1. `ready`, before anything else this document will see.
		SendReady(a_viewId);
		// Mark greeted before replay so PublishState can deliver current values.
		if (m_onHello) {
			m_onHello(a_viewId);
		}
		// Open events only after replay so listener-raised events cannot overtake the backlog.
		auto& live = m_gates[std::string(a_viewId)];
		auto queued = std::move(live.queued);
		live.queued.clear();
		live.eventsOpen = true;
		for (const auto& encoded : queued) {
			if (m_send) {
				m_send(a_viewId, encoded);
			}
		}
		REX::DEBUG("MessageBridge: view '{}' greeted — ready, state replay, {} queued event(s), events open", a_viewId, queued.size());
	}

	void MessageBridge::Respond(const nlohmann::json& a_payload)
	{
		if (m_currentRequestId.empty()) {
			REX::WARN("MessageBridge: Respond() outside a request ('{}')", m_currentName);
			return;
		}
		if (m_settled) {
			REX::WARN("MessageBridge: '{}' settled twice — ignoring the second answer", m_currentName);
			return;
		}
		m_settled = true;
		if (m_send && !m_currentSource.empty()) {
			m_send(m_currentSource, EncodeReply(m_currentRequestId, Json::Dump(a_payload)));
		}
		NoteTracedReply("reply");
	}

	void MessageBridge::Reject(std::string_view a_code, std::string_view a_message)
	{
		if (m_currentRequestId.empty()) {
			// Send endpoints have no settlement channel; use a request for outcomes.
			REX::WARN("MessageBridge: Reject('{}') outside a request ('{}')", a_code, m_currentName);
			return;
		}
		if (m_settled) {
			REX::WARN("MessageBridge: '{}' settled twice — ignoring the second answer", m_currentName);
			return;
		}
		m_settled = true;
		if (m_send && !m_currentSource.empty()) {
			m_send(m_currentSource, EncodeError(m_currentRequestId, a_code, a_message));
		}
		NoteTracedReply(std::string("error:") + std::string(a_code));
	}

	MessageBridge::DeferToken MessageBridge::Defer()
	{
		if (m_currentRequestId.empty() || m_settled) {
			REX::WARN("MessageBridge: Defer() outside an unsettled request ('{}')", m_currentName);
			return 0;
		}
		m_settled = true;
		// Use a runtime token because page correlation ids are only document-local.
		const DeferToken token = g_nextDeferToken.fetch_add(1, std::memory_order_relaxed);
		m_pending[token] = Pending{
			.view = m_currentSource,
			.requestId = m_currentRequestId,
			.name = m_currentName,
			.deadline = std::chrono::steady_clock::now() + kRequestDeadline,
		};
		NoteTracedReply("deferred");
		return token;
	}

	void MessageBridge::RespondTo(DeferToken a_token, const nlohmann::json& a_payload)
	{
		RespondJsonTo(a_token, Json::Dump(a_payload));
	}

	void MessageBridge::RespondJsonTo(DeferToken a_token, std::string_view a_payloadJson)
	{
		const auto it = m_pending.find(a_token);
		if (it == m_pending.end()) {
			// Never deliver late or duplicate settlements, but keep them visible in logs.
			REX::DEBUG("MessageBridge: dropped a reply for '{}' — already settled, expired, or its view is gone",
				a_token);
			return;
		}
		const auto view = it->second.view;
		const auto requestId = it->second.requestId;
		m_pending.erase(it);
		if (m_send && !view.empty()) {
			m_send(view, EncodeReply(requestId, a_payloadJson));
		}
		NoteTracedReply("reply");
	}

	void MessageBridge::RejectTo(DeferToken a_token, std::string_view a_code, std::string_view a_message)
	{
		const auto it = m_pending.find(a_token);
		if (it == m_pending.end()) {
			REX::DEBUG("MessageBridge: dropped a '{}' rejection for '{}' — already settled, expired, or its view is gone",
				a_code, a_token);
			return;
		}
		const auto view = it->second.view;
		const auto requestId = it->second.requestId;
		m_pending.erase(it);
		if (m_send && !view.empty()) {
			m_send(view, EncodeError(requestId, a_code, a_message));
		}
		NoteTracedReply(std::string("error:") + std::string(a_code));
	}

	void MessageBridge::Emit(std::string_view a_viewId, std::string_view a_name, const nlohmann::json& a_payload)
	{
		EmitJson(a_viewId, a_name, Json::Dump(a_payload));
	}

	void MessageBridge::EmitJson(std::string_view a_viewId, std::string_view a_name, std::string_view a_payloadJson)
	{
		if (!m_send || a_viewId.empty()) {
			return;
		}
		DeliverEvent(a_viewId, EncodeEvent(a_name, a_payloadJson), a_name);
	}

	void MessageBridge::Emit(const std::unordered_set<std::string>& a_viewIds, std::string_view a_name, const nlohmann::json& a_payload)
	{
		EmitJson(a_viewIds, a_name, Json::Dump(a_payload));
	}

	void MessageBridge::EmitJson(const std::unordered_set<std::string>& a_viewIds, std::string_view a_name, std::string_view a_payloadJson)
	{
		if (!m_send || a_viewIds.empty()) {
			return;
		}
		// Encode once, hand the same text to every target transport.
		const auto encoded = EncodeEvent(a_name, a_payloadJson);
		for (const auto& id : a_viewIds) {
			if (!id.empty()) {
				DeliverEvent(id, encoded, a_name);
			}
		}
	}

	void MessageBridge::DeliverEvent(std::string_view a_viewId, const std::string& a_encoded, std::string_view a_name)
	{
		auto& gate = m_gates[std::string(a_viewId)];
		if (!gate.eventsOpen) {
			if (gate.queued.size() >= kMaxQueuedEventsPerView) {
				gate.queued.pop_front();
				REX::WARN("MessageBridge: view '{}' has not greeted yet — dropped the oldest queued event "
						  "to make room for '{}'", a_viewId, a_name);
			}
			gate.queued.push_back(a_encoded);
			return;
		}
		NoteTracedReply(a_name);
		m_send(a_viewId, a_encoded);
	}

	void MessageBridge::PublishState(std::string_view a_viewId, std::string_view a_mod, std::string_view a_key,
		const nlohmann::json& a_value)
	{
		PublishJsonState(a_viewId, a_mod, a_key, Json::Dump(a_value));
	}

	void MessageBridge::PublishJsonState(std::string_view a_viewId, std::string_view a_mod, std::string_view a_key,
		std::string_view a_valueJson)
	{
		if (!m_send || a_viewId.empty()) {
			return;
		}
		// Pre-hello documents receive state through replay; do not queue stale values.
		const auto it = m_gates.find(std::string(a_viewId));
		if (it == m_gates.end() || !it->second.greeted) {
			return;
		}
		if (!m_inMessage && IsTracedState(a_key)) {
			REX::DEBUG("MessageBridge: state '{}/{}' -> view '{}'", a_mod, a_key, a_viewId);
		} else {
			NoteTracedReply(std::format("state:{}", a_key));
		}
		m_send(a_viewId, EncodeState(a_mod, a_key, a_valueJson));
	}

	void MessageBridge::PublishState(const std::unordered_set<std::string>& a_viewIds, std::string_view a_mod,
		std::string_view a_key, const nlohmann::json& a_value)
	{
		if (!m_send || a_viewIds.empty()) {
			return;
		}
		const auto valueJson = Json::Dump(a_value);
		for (const auto& id : a_viewIds) {
			PublishJsonState(id, a_mod, a_key, valueJson);
		}
	}

	void MessageBridge::SendReady(std::string_view a_viewId)
	{
		if (!m_send || a_viewId.empty()) {
			return;
		}
		// Ready identifies the runtime release, bridge protocol, view, and owning mod.
		const nlohmann::json payload{
			{ "game", "Starfield" },
			{ "plugin", kPluginName },
			{ "version", kOsfuiReleaseVersion },
			{ "bridgeVersion", kBridgeProtocolVersion },
			{ "view", std::string(a_viewId) },
			{ "mod", std::string(Ids::ModOf(a_viewId)) },
		};
		std::string message;
		const auto payloadJson = Json::Dump(payload);
		message.reserve(payloadJson.size() + 32);
		message += R"({"kind":"ready","payload":)";
		message += payloadJson;
		message += '}';
		m_send(a_viewId, message);
		REX::DEBUG("MessageBridge: ready -> view '{}'", a_viewId);
	}

	void MessageBridge::OnViewCreated(std::string_view a_viewId)
	{
		OnViewDestroyed(a_viewId);  // retire the previous document and all its deferred tokens
		// Arm a closed gate so events wait for the new document's hello.
		auto& gate = m_gates[std::string(a_viewId)];
		gate.greeted = false;
		gate.eventsOpen = false;
		gate.queued.clear();
	}

	void MessageBridge::OnViewDestroyed(std::string_view a_viewId)
	{
		m_gates.erase(std::string(a_viewId));
		// Reap deferred requests whose destination view no longer exists; their late answers are ignored.
		for (auto it = m_pending.begin(); it != m_pending.end();) {
			if (it->second.view == a_viewId) {
				REX::DEBUG("MessageBridge: reaped in-flight request '{}' — view '{}' went away",
					it->second.name, a_viewId);
				it = m_pending.erase(it);
			} else {
				++it;
			}
		}
	}

	void MessageBridge::RejectAll(std::string_view a_code, std::string_view a_message)
	{
		if (m_pending.empty()) return;
		auto pending = std::exchange(m_pending, {});
		REX::DEBUG("MessageBridge: rejected {} in-flight request(s) with '{}'", pending.size(), a_code);
		for (const auto& [_, req] : pending) {
			if (m_send && !req.view.empty()) {
				m_send(req.view, EncodeError(req.requestId, a_code, a_message));
			}
		}
	}

	void MessageBridge::Tick(std::chrono::steady_clock::time_point a_now)
	{
		if (m_pending.empty()) {
			return;
		}
		std::vector<Pending> expired;
		for (auto it = m_pending.begin(); it != m_pending.end();) {
			if (a_now >= it->second.deadline) {
				expired.emplace_back(it->second);
				it = m_pending.erase(it);
			} else {
				++it;
			}
		}
		for (const auto& req : expired) {
			REX::WARN("MessageBridge: '{}' from view '{}' missed the {}s OSF UI runtime deadline",
				req.name, req.view, std::chrono::duration_cast<std::chrono::seconds>(kRequestDeadline).count());
			// Settle with the page's correlation id, not the runtime map token.
			if (m_send && !req.view.empty()) {
				m_send(req.view, EncodeError(req.requestId, "no-response", "the endpoint handler never answered"));
			}
			// Handler silence is reported to the page but never counted against the view.
			ReportProtocolFault(req.view, "no-response", "the endpoint handler never answered", { { "name", req.name } },
				/*a_viewFault*/ false);
		}
	}

	void MessageBridge::ReportProtocolFault(std::string_view a_viewId, std::string_view a_code, std::string_view a_message,
		const nlohmann::json& a_detail, bool a_viewFault)
	{
		REX::WARN("MessageBridge: [content] view '{}': {} — {}", a_viewId, a_code, a_message);
		if (m_protocolFaultSink) {
			m_protocolFaultSink(a_viewId, a_code, a_message, a_detail, a_viewFault);
		}
	}

	void MessageBridge::NoteTracedReply(std::string_view a_what)
	{
		if (!m_inMessage) {
			return;  // unsolicited push: not part of any message's trace
		}
		// Bound trace growth from handlers that attempt repeated settlement.
		constexpr std::size_t kMaxTraceLength = 160;
		if (m_trace.size() >= kMaxTraceLength) {
			if (!m_trace.ends_with("...")) {
				m_trace += ", ...";
			}
			return;
		}
		if (!m_trace.empty()) {
			m_trace += ", ";
		}
		m_trace.append(a_what);
	}

	// Splice serialized payloads into fixed-key-order envelopes without a deep copy.

	std::string MessageBridge::EncodeEvent(std::string_view a_name, std::string_view a_payloadJson)
	{
		const auto name = Quote(BoundedEcho(a_name, Ids::kMaxModIdLen + 1 + kMaxEndpointNameLength));
		std::string message;
		message.reserve(a_payloadJson.size() + name.size() + 40);
		message += R"({"kind":"event","name":)";
		message += name;
		message += R"(,"payload":)";
		message += a_payloadJson.empty() ? std::string_view{ "{}" } : a_payloadJson;
		message += '}';
		return message;
	}

	std::string MessageBridge::EncodeState(std::string_view a_mod, std::string_view a_key, std::string_view a_valueJson)
	{
		const auto mod = Quote(BoundedEcho(a_mod));
		const auto key = Quote(BoundedEcho(a_key));
		std::string message;
		message.reserve(a_valueJson.size() + mod.size() + key.size() + 48);
		message += R"({"kind":"state","mod":)";
		message += mod;
		message += R"(,"key":)";
		message += key;
		message += R"(,"value":)";
		message += a_valueJson.empty() ? std::string_view{ "null" } : a_valueJson;
		message += '}';
		return message;
	}

	std::string MessageBridge::EncodeReply(std::string_view a_requestId, std::string_view a_payloadJson)
	{
		const auto id = Quote(a_requestId);
		std::string message;
		message.reserve(a_payloadJson.size() + id.size() + 40);
		message += R"({"kind":"reply","id":)";
		message += id;
		message += R"(,"payload":)";
		message += a_payloadJson.empty() ? std::string_view{ "{}" } : a_payloadJson;
		message += '}';
		return message;
	}

	std::string MessageBridge::EncodeError(std::string_view a_requestId, std::string_view a_code, std::string_view a_message)
	{
		const auto id = Quote(a_requestId);
		const auto code = Quote(BoundedEcho(a_code));
		// Bound author text on a UTF-8 codepoint boundary before dump().
		const auto message = Quote(BoundedEcho(a_message));
		std::string out;
		out.reserve(id.size() + code.size() + message.size() + 56);
		out += R"({"kind":"error","id":)";
		out += id;
		out += R"(,"payload":{"code":)";
		out += code;
		out += R"(,"message":)";
		out += message;
		out += "}}";
		return out;
	}
}
