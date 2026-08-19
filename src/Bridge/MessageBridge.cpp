#include "Bridge/MessageBridge.h"

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

		// Keep the runtime deadline longer than the page timer so timeout and no-response stay distinct.
		constexpr auto kRequestDeadline = std::chrono::seconds(30);

		[[nodiscard]] std::string BoundedEcho(std::string_view a_s)
		{
			return std::string{ a_s.substr(0, StringUtil::Utf8TruncateLen(a_s, kMaxEndpointNameLength)) };
		}

		class ScopeExit
		{
		public:
			explicit ScopeExit(std::function<void()> a_fn) : _fn(std::move(a_fn)) {}
			~ScopeExit() { _fn(); }

			ScopeExit(const ScopeExit&) = delete;
			ScopeExit& operator=(const ScopeExit&) = delete;

		private:
			std::function<void()> _fn;
		};

		// JSON string literal for a value we control the bounds of.
		[[nodiscard]] std::string Quote(std::string_view a_s)
		{
			return Json::Dump(nlohmann::json(std::string(a_s)));
		}

		// Trace only low-volume unsolicited pushes; settlements fold into their inbound trace.
		bool IsTracedState(std::string_view a_key)
		{
			return a_key == "settings" || a_key == "views" || a_key == "diagnostics" || a_key == "i18n" ||
			       a_key == "keybindings" || a_key == "input-context";
		}
	}

	MessageBridge::MessageBridge(SendFn a_send) :
		_send(std::move(a_send))
	{}

	void MessageBridge::RegisterSend(std::string a_name, SendHandler a_handler)
	{
		if (_requests.contains(a_name) || _commands.contains(a_name)) {
			REX::WARN("MessageBridge: [content] refused send endpoint '{}' — name already registered", a_name);
			return;
		}
		_sends[std::move(a_name)] = std::move(a_handler);
	}

	bool MessageBridge::RegisterRequest(std::string a_name, RequestHandler a_handler)
	{
		if (_sends.contains(a_name) || _commands.contains(a_name)) {
			REX::WARN("MessageBridge: [content] refused request endpoint '{}' — name already registered", a_name);
			return false;
		}
		// BridgeApi owns public first-wins policy; this internal trampoline is replaceable.
		_requests[std::move(a_name)] = std::move(a_handler);
		return true;
	}

	bool MessageBridge::RegisterCommand(std::string a_name, SendHandler a_handler)
	{
		if (_sends.contains(a_name) || _requests.contains(a_name)) {
			REX::WARN("MessageBridge: [content] refused command '{}' — name already registered", a_name);
			return false;
		}
		_commands[std::move(a_name)] = std::move(a_handler);
		return true;
	}

	void MessageBridge::UnregisterSend(std::string_view a_name)
	{
		_sends.erase(std::string(a_name));
	}

	void MessageBridge::UnregisterRequest(std::string_view a_name)
	{
		_requests.erase(std::string(a_name));
	}

	void MessageBridge::UnregisterCommand(std::string_view a_name)
	{
		_commands.erase(std::string(a_name));
	}

	void MessageBridge::HandleWebMessage(std::string_view a_viewId, std::string_view a_json)
	{
		// Remember the source so settlements route back to it.
		_currentSource = std::string(a_viewId);
		_currentRequestId.clear();
		_currentName.clear();
		_settled = false;
		_inMessage = true;
		_trace.clear();

		// The transport catches handler exceptions above us. This guard still restores the in-flight context while the exception unwinds, so a later Respond/Reject cannot accidentally settle the abandoned request.
		const ScopeExit cleanup([this] {
			_inMessage = false;
			REX::DEBUG("MessageBridge: '{}' from view '{}' -> {}", _currentName, _currentSource, _trace.empty() ? std::string_view{ "(nothing)" } : std::string_view{ _trace });
			_currentSource.clear();
			_currentRequestId.clear();
			_currentName.clear();
			_settled = false;
			_trace.clear();
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
		_currentName = name;

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
			DispatchSend(name, payload);
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

	void MessageBridge::DispatchSend(const std::string& a_name, const nlohmann::json& a_payload)
	{
		if (a_name == "osfui.hello") {
			HandleHello(_currentSource);
			NoteTracedReply("ready+state");
			return;
		}
		if (const auto it = _sends.find(a_name); it != _sends.end()) {
			it->second(a_payload, *this);
			return;
		}
		if (const auto it = _commands.find(a_name); it != _commands.end()) {
			it->second(a_payload, *this);
			return;
		}
		// Drop and report wrong-kind sends rather than executing the mutation.
		if (_requests.contains(a_name)) {
			ReportProtocolFault(_currentSource, "wrong-endpoint-kind",
				std::format("'{}' is a request endpoint — use request(), not send()", a_name),
				{ { "name", a_name } });
			NoteTracedReply("wrong-endpoint-kind");
			return;
		}
		// Deduplicate log warnings while still reporting every fault to the page.
		constexpr std::size_t kMaxWarnedEndpoints = 512;
		if (_warnedUnknownEndpoints.size() < kMaxWarnedEndpoints &&
			_warnedUnknownEndpoints.insert(a_name).second) {
			REX::WARN("MessageBridge: [content] dropped send to unknown endpoint '{}' "
					  "(further drops of this endpoint are not logged)", a_name);
		}
		ReportProtocolFault(_currentSource, "unknown-endpoint", "no such endpoint", { { "name", a_name } });
		NoteTracedReply("unknown-endpoint");
	}

	void MessageBridge::DispatchRequest(const std::string& a_name, const std::string& a_id, const nlohmann::json& a_payload)
	{
		_currentRequestId = a_id;

		const auto it = _requests.find(a_name);
		if (it == _requests.end()) {
			if (const auto command = _commands.find(a_name);
				command != _commands.end()) {
				auto payload = a_payload;
				payload["requestId"] = a_id;
				command->second(payload, *this);
				if (!_settled) {
					Respond(nlohmann::json{ { "ok", true }, { "command", a_name } });
				}
				return;
			}
			if (const auto send = _sends.find(a_name);
				send != _sends.end() && IsLegacyApiView(_currentSource)) {
				// Explicit legacy documents retain the 1.x uniform command acknowledgement.
				send->second(a_payload, *this);
				if (!_settled) {
					Respond(nlohmann::json{ { "ok", true }, { "command", a_name } });
				}
				return;
			}
			if (_sends.contains(a_name)) {
				Reject("wrong-endpoint-kind",
					std::format("'{}' is a send endpoint — use send(), not request()", a_name));
				return;
			}
			constexpr std::size_t kMaxWarnedEndpoints = 512;
			if (_warnedUnknownEndpoints.size() < kMaxWarnedEndpoints &&
				_warnedUnknownEndpoints.insert(a_name).second) {
				REX::WARN("MessageBridge: [content] rejected request to unknown endpoint '{}' "
						  "(further rejections of this endpoint are not logged)", a_name);
			}
			Reject("unknown-endpoint", "no such endpoint");
			return;
		}

		// Refuse saturated views before invoking the endpoint handler.
		std::size_t owned = 0;
		for (const auto& [_, req] : _pending) {
			if (req.view == _currentSource) {
				++owned;
			}
		}
		if (owned >= kMaxPendingRequestsPerView) {
			REX::WARN("MessageBridge: [content] view '{}' has {} requests in flight — refusing '{}'",
				_currentSource, owned, a_name);
			Reject("request-capacity", "too many requests are already in flight for this view");
			return;
		}

		it->second(a_payload, *this);
		if (!_settled) {
			// An endpoint returning without settlement is a platform bug, not a silent hang.
			REX::ERROR("MessageBridge: request endpoint '{}' returned without settling", a_name);
			Reject("internal", "the endpoint did not answer");
		}
	}

	void MessageBridge::HandleHello(std::string_view a_viewId)
	{
		// Preserve pre-hello events here; only OnViewCreated discards an old queue.
		_gates[std::string(a_viewId)].greeted = true;

		// 1. `ready`, before anything else this document will see.
		SendReady(a_viewId);
		// Mark greeted before replay so PublishState can deliver current values.
		if (_onHello) {
			_onHello(a_viewId);
		}
		// Open events only after replay so listener-raised events cannot overtake the backlog.
		auto& live = _gates[std::string(a_viewId)];
		auto queued = std::move(live.queued);
		live.queued.clear();
		live.eventsOpen = true;
		for (const auto& encoded : queued) {
			if (_send) {
				_send(a_viewId, encoded);
			}
		}
		REX::DEBUG("MessageBridge: view '{}' greeted — ready, state replay, {} queued event(s), events open",
			a_viewId, queued.size());
	}

	void MessageBridge::Respond(const nlohmann::json& a_payload)
	{
		if (_currentRequestId.empty()) {
			REX::WARN("MessageBridge: Respond() outside a request ('{}')", _currentName);
			return;
		}
		if (_settled) {
			REX::WARN("MessageBridge: '{}' settled twice — ignoring the second answer", _currentName);
			return;
		}
		_settled = true;
		if (_send && !_currentSource.empty()) {
			_send(_currentSource, EncodeReply(_currentRequestId, Json::Dump(a_payload)));
		}
		NoteTracedReply("reply");
	}

	void MessageBridge::Reject(std::string_view a_code, std::string_view a_message)
	{
		if (_currentRequestId.empty()) {
			// Send endpoints have no settlement channel; use a request for outcomes.
			REX::WARN("MessageBridge: Reject('{}') outside a request ('{}')", a_code, _currentName);
			return;
		}
		if (_settled) {
			REX::WARN("MessageBridge: '{}' settled twice — ignoring the second answer", _currentName);
			return;
		}
		_settled = true;
		if (_send && !_currentSource.empty()) {
			_send(_currentSource, EncodeError(_currentRequestId, a_code, a_message));
		}
		NoteTracedReply(std::string("error:") + std::string(a_code));
	}

	std::string MessageBridge::Defer(DeferredDropHandler a_onDropped)
	{
		if (_currentRequestId.empty() || _settled) {
			REX::WARN("MessageBridge: Defer() outside an unsettled request ('{}')", _currentName);
			return {};
		}
		_settled = true;
		// Use a runtime token because page correlation ids are only document-local.
		auto token = "d" + std::to_string(_nextDeferToken++);
		_pending[token] = Pending{
			.view = _currentSource,
			.requestId = _currentRequestId,
			.name = _currentName,
			.deadline = std::chrono::steady_clock::now() + kRequestDeadline,
			.onDropped = std::move(a_onDropped),
		};
		NoteTracedReply("deferred");
		return token;
	}

	void MessageBridge::RespondTo(std::string_view a_token, const nlohmann::json& a_payload)
	{
		RespondJsonTo(a_token, Json::Dump(a_payload));
	}

	void MessageBridge::RespondJsonTo(std::string_view a_token, std::string_view a_payloadJson)
	{
		const auto it = _pending.find(std::string(a_token));
		if (it == _pending.end()) {
			// Never deliver late or duplicate settlements, but keep them visible in logs.
			REX::DEBUG("MessageBridge: dropped a reply for '{}' — already settled, expired, or its view is gone",
				a_token);
			return;
		}
		const auto view = it->second.view;
		const auto requestId = it->second.requestId;
		_pending.erase(it);
		if (_send && !view.empty()) {
			_send(view, EncodeReply(requestId, a_payloadJson));
		}
		NoteTracedReply("reply");
	}

	void MessageBridge::RejectTo(std::string_view a_token, std::string_view a_code, std::string_view a_message)
	{
		const auto it = _pending.find(std::string(a_token));
		if (it == _pending.end()) {
			REX::DEBUG("MessageBridge: dropped a '{}' rejection for '{}' — already settled, expired, or its view is gone",
				a_code, a_token);
			return;
		}
		const auto view = it->second.view;
		const auto requestId = it->second.requestId;
		_pending.erase(it);
		if (_send && !view.empty()) {
			_send(view, EncodeError(requestId, a_code, a_message));
		}
		NoteTracedReply(std::string("error:") + std::string(a_code));
	}

	void MessageBridge::Emit(std::string_view a_viewId, std::string_view a_name, const nlohmann::json& a_payload)
	{
		EmitJson(a_viewId, a_name, Json::Dump(a_payload));
	}

	void MessageBridge::EmitJson(std::string_view a_viewId, std::string_view a_name, std::string_view a_payloadJson)
	{
		if (!_send || a_viewId.empty()) {
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
		if (!_send || a_viewIds.empty()) {
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
		auto& gate = _gates[std::string(a_viewId)];
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
		_send(a_viewId, a_encoded);
	}

	void MessageBridge::PublishState(std::string_view a_viewId, std::string_view a_mod, std::string_view a_key,
		const nlohmann::json& a_value)
	{
		PublishJsonState(a_viewId, a_mod, a_key, Json::Dump(a_value));
	}

	void MessageBridge::PublishJsonState(std::string_view a_viewId, std::string_view a_mod, std::string_view a_key,
		std::string_view a_valueJson)
	{
		if (!_send || a_viewId.empty()) {
			return;
		}
		// Pre-hello documents receive state through replay; do not queue stale values.
		const auto it = _gates.find(std::string(a_viewId));
		if (it == _gates.end() || !it->second.greeted) {
			return;
		}
		if (!_inMessage && IsTracedState(a_key)) {
			REX::DEBUG("MessageBridge: state '{}/{}' -> view '{}'", a_mod, a_key, a_viewId);
		} else {
			NoteTracedReply(std::format("state:{}", a_key));
		}
		_send(a_viewId, EncodeState(a_mod, a_key, a_valueJson));
	}

	void MessageBridge::PublishState(const std::unordered_set<std::string>& a_viewIds, std::string_view a_mod,
		std::string_view a_key, const nlohmann::json& a_value)
	{
		if (!_send || a_viewIds.empty()) {
			return;
		}
		const auto valueJson = Json::Dump(a_value);
		for (const auto& id : a_viewIds) {
			PublishJsonState(id, a_mod, a_key, valueJson);
		}
	}

	void MessageBridge::PublishStateAll(std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value)
	{
		if (!_send || _gates.empty()) {
			return;
		}
		const auto valueJson = Json::Dump(a_value);
		for (const auto& [view, gate] : _gates) {
			if (gate.greeted) {
				PublishJsonState(view, a_mod, a_key, valueJson);
			}
		}
	}

	void MessageBridge::EmitAll(std::string_view a_name, const nlohmann::json& a_payload)
	{
		if (!_send || _gates.empty()) {
			return;
		}
		// Broadcast one-shot events only to documents already present and greeted.
		const auto encoded = EncodeEvent(a_name, Json::Dump(a_payload));
		for (const auto& [view, gate] : _gates) {
			if (gate.eventsOpen) {
				NoteTracedReply(a_name);
				_send(view, encoded);
			}
		}
	}

	void MessageBridge::SendReady(std::string_view a_viewId)
	{
		if (!_send || a_viewId.empty()) {
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
		_send(a_viewId, message);
		REX::DEBUG("MessageBridge: ready -> view '{}'", a_viewId);
	}

	void MessageBridge::OnViewCreated(std::string_view a_viewId, bool a_legacyApi)
	{
		// Arm a closed gate so events wait for the new document's hello.
		auto& gate = _gates[std::string(a_viewId)];
		gate.greeted = false;
		gate.eventsOpen = false;
		gate.legacyApi = a_legacyApi;
		gate.queued.clear();
	}

	void MessageBridge::OnViewDestroyed(std::string_view a_viewId)
	{
		_gates.erase(std::string(a_viewId));
		// Reap deferred requests whose destination view no longer exists.
		std::vector<DeferredDropHandler> dropped;
		for (auto it = _pending.begin(); it != _pending.end();) {
			if (it->second.view == a_viewId) {
				REX::DEBUG("MessageBridge: reaped in-flight request '{}' — view '{}' went away",
					it->second.name, a_viewId);
				if (it->second.onDropped) dropped.push_back(std::move(it->second.onDropped));
				it = _pending.erase(it);
			} else {
				++it;
			}
		}
		for (auto& cleanup : dropped) cleanup();
	}

	bool MessageBridge::IsLegacyApiView(std::string_view a_viewId) const
	{
		const auto it = _gates.find(std::string(a_viewId));
		return it != _gates.end() && it->second.legacyApi;
	}

	void MessageBridge::Tick(std::chrono::steady_clock::time_point a_now)
	{
		if (_pending.empty()) {
			return;
		}
		std::vector<Pending> expired;
		for (auto it = _pending.begin(); it != _pending.end();) {
			if (a_now >= it->second.deadline) {
				expired.emplace_back(it->second);
				it = _pending.erase(it);
			} else {
				++it;
			}
		}
		for (const auto& req : expired) {
			if (req.onDropped) req.onDropped();
			REX::WARN("MessageBridge: '{}' from view '{}' missed the {}s OSF UI runtime deadline",
				req.name, req.view, std::chrono::duration_cast<std::chrono::seconds>(kRequestDeadline).count());
			// Settle with the page's correlation id, not the runtime map token.
			if (_send && !req.view.empty()) {
				_send(req.view, EncodeError(req.requestId, "no-response", "the endpoint handler never answered"));
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
		if (_protocolFaultSink) {
			_protocolFaultSink(a_viewId, a_code, a_message, a_detail, a_viewFault);
		}
	}

	void MessageBridge::NoteTracedReply(std::string_view a_what)
	{
		if (!_inMessage) {
			return;  // unsolicited push: not part of any message's trace
		}
		// Bound trace growth from handlers that attempt repeated settlement.
		constexpr std::size_t kMaxTraceLength = 160;
		if (_trace.size() >= kMaxTraceLength) {
			if (!_trace.ends_with("...")) {
				_trace += ", ...";
			}
			return;
		}
		if (!_trace.empty()) {
			_trace += ", ";
		}
		_trace.append(a_what);
	}

	// Splice serialized payloads into fixed-key-order envelopes without a deep copy.

	std::string MessageBridge::EncodeEvent(std::string_view a_name, std::string_view a_payloadJson)
	{
		const auto name = Quote(BoundedEcho(a_name));
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
