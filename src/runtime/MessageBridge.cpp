#include "runtime/MessageBridge.h"

#include "core/StringUtil.h"
#include "core/Version.h"
#include "runtime/Ids.h"
#include "runtime/Json.h"

namespace OSFUI
{
	namespace
	{
		// Correlation ids are caller-chosen opaque strings, echoed back
		// verbatim. Bounded because the inbound payload is untrusted. Unlike
		// 1.x, an over-long or non-string id is NOT demoted to fire-and-forget:
		// silent demotion turned a client bug into a request that never
		// settles. It is a hard `invalid-request` instead.
		constexpr std::size_t kMaxRequestIdLength = 64;

		// Endpoint names are view-supplied and get echoed back inside error
		// payloads and debug events. Bound them on a codepoint boundary: they
		// are dumped by the encoders, and dump() throws type_error.316 on a
		// split UTF-8 sequence — the caller here is the web-message callback,
		// which runs with no handler between it and std::terminate.
		constexpr std::size_t kMaxEchoedNameLength = 128;

		// Events emitted before a document greets the bridge are held per view.
		// This is what preserves the native ABI's message-before-first-paint
		// guarantee (RegisterView -> SendToWeb -> RequestMenu in one tick) now
		// that the handshake is page-initiated. Overflow drops the OLDEST: an
		// event is a one-shot happening, and the newest are the ones still
		// worth delivering.
		constexpr std::size_t kMaxQueuedEventsPerView = 64;

		// Concurrent deferred requests one view may own. A page looping on
		// request() against a slow backend hits `request-capacity` instead of
		// growing host memory without bound.
		constexpr std::size_t kMaxPendingRequestsPerView = 64;

		// Host-side deadline for a deferred request. Longer than the helper's
		// 10 s client timer on purpose: the two are distinguishable failures
		// (`timeout` is "the page gave up", `no-response` is "the backend never
		// answered"), and collapsing them would hide which side is broken.
		constexpr auto kRequestDeadline = std::chrono::seconds(30);

		[[nodiscard]] std::string BoundedEcho(std::string_view a_s)
		{
			return std::string{ a_s.substr(0, StringUtil::Utf8TruncateLen(a_s, kMaxEchoedNameLength)) };
		}

		// JSON string literal for a value we control the bounds of.
		[[nodiscard]] std::string Quote(std::string_view a_s)
		{
			return Json::Dump(nlohmann::json(std::string(a_s)));
		}

		// Outbound trace allowlist for UNSOLICITED pushes only: the boot
		// handshake plus platform state. Settlements of an in-flight message
		// never reach this — they fold into that message's own trace line.
		// Deliberately not every name: ui.gamepad and friends push far too
		// often to log.
		bool IsTracedState(std::string_view a_key)
		{
			return a_key == "settings" || a_key == "views" || a_key == "diagnostics" || a_key == "i18n";
		}
	}

	MessageBridge::MessageBridge(SendFn a_send) :
		_send(std::move(a_send))
	{}

	// -----------------------------------------------------------------------
	// registry
	// -----------------------------------------------------------------------

	void MessageBridge::RegisterSend(std::string a_name, SendHandler a_handler)
	{
		if (_requests.contains(a_name) || _compatCommands.contains(a_name)) {
			REX::WARN("MessageBridge: [content] refused send endpoint '{}' — name already registered", a_name);
			return;
		}
		_sends[std::move(a_name)] = std::move(a_handler);
	}

	bool MessageBridge::RegisterRequest(std::string a_name, RequestHandler a_handler)
	{
		if (_sends.contains(a_name) || _compatCommands.contains(a_name)) {
			REX::WARN("MessageBridge: [content] refused request endpoint '{}' — name already registered", a_name);
			return false;
		}
		// BridgeApi owns public first-wins policy. Re-application here replaces
		// the internal trampoline after an explicit unregister/re-register.
		_requests[std::move(a_name)] = std::move(a_handler);
		return true;
	}

	void MessageBridge::RegisterCompatCommand(std::string a_name, SendHandler a_handler)
	{
		if (_sends.contains(a_name) || _requests.contains(a_name)) {
			REX::WARN("MessageBridge: [content] refused compatibility command '{}' — name already registered", a_name);
			return;
		}
		_compatCommands[std::move(a_name)] = std::move(a_handler);
	}

	void MessageBridge::UnregisterSend(std::string_view a_name)
	{
		_sends.erase(std::string(a_name));
	}

	void MessageBridge::UnregisterRequest(std::string_view a_name)
	{
		_requests.erase(std::string(a_name));
	}

	void MessageBridge::UnregisterCompatCommand(std::string_view a_name)
	{
		_compatCommands.erase(std::string(a_name));
	}

	bool MessageBridge::HasSend(std::string_view a_name) const
	{
		return _sends.contains(std::string(a_name));
	}

	bool MessageBridge::HasRequest(std::string_view a_name) const
	{
		return _requests.contains(std::string(a_name));
	}

	bool MessageBridge::HasCompatCommand(std::string_view a_name) const
	{
		return _compatCommands.contains(std::string(a_name));
	}

	// -----------------------------------------------------------------------
	// inbound
	// -----------------------------------------------------------------------

	void MessageBridge::HandleWebMessage(std::string_view a_viewId, std::string_view a_json)
	{
		// Remember the source so settlements route back to it.
		_currentSource = std::string(a_viewId);
		_currentRequestId.clear();
		_currentName.clear();
		_settled = false;
		_inMessage = true;
		_trace.clear();

		// Every early return below lands on the same exit path, so the trace
		// line and the in-flight context are cleaned up exactly once.
		const auto finish = [this] {
			_inMessage = false;
			REX::DEBUG("MessageBridge: '{}' from view '{}' -> {}", _currentName, _currentSource,
				_trace.empty() ? std::string_view{ "(nothing)" } : std::string_view{ _trace });
			_currentRequestId.clear();
			_currentName.clear();
			_settled = false;
		};

		const auto msg = Json::Parse(a_json, "web->native message");
		if (!msg || !msg->is_object()) {
			// Unparseable: there is no id to correlate an error to, so the only
			// honest channel is the log plus the offending view's own console.
			Surface(a_viewId, "invalid-request", "malformed message", {});
			NoteTracedReply("invalid-request");
			finish();
			return;
		}

		const auto kind = Json::GetString(*msg, "kind", "");
		const auto name = BoundedEcho(Json::GetString(*msg, "name", ""));
		_currentName = name;

		if (kind != "send" && kind != "request") {
			Surface(a_viewId, "invalid-request", "kind must be \"send\" or \"request\"",
				{ { "kind", BoundedEcho(kind) }, { "name", name } });
			NoteTracedReply("invalid-request");
			finish();
			return;
		}
		if (name.empty()) {
			Surface(a_viewId, "invalid-request", "a message needs a non-empty endpoint name",
				{ { "kind", kind } });
			NoteTracedReply("invalid-request");
			finish();
			return;
		}

		// Routing metadata sits beside the payload, so a payload field can
		// never override it. A present-but-non-object payload is a client bug,
		// not something to coerce.
		nlohmann::json payload = nlohmann::json::object();
		if (const auto it = msg->find("payload"); it != msg->end() && !it->is_null()) {
			if (!it->is_object()) {
				Surface(a_viewId, "invalid-request", "payload must be an object",
					{ { "kind", kind }, { "name", name } });
				NoteTracedReply("invalid-request");
				finish();
				return;
			}
			payload = *it;
		}

		const auto idIt = msg->find("id");
		const bool hasId = idIt != msg->end() && !idIt->is_null();

		if (kind == "send") {
			// `id` is forbidden on a send: a caller that supplied one expects a
			// settlement it will never get, and answering one would resurrect
			// the 1.x auto-ack.
			if (hasId) {
				Surface(a_viewId, "invalid-request", "send messages carry no id — use a request",
					{ { "name", name } });
				NoteTracedReply("invalid-request");
				finish();
				return;
			}
			DispatchSend(name, payload);
			finish();
			return;
		}

		if (!hasId || !idIt->is_string()) {
			Surface(a_viewId, "invalid-request", "requests carry a string id",
				{ { "name", name } });
			NoteTracedReply("invalid-request");
			finish();
			return;
		}
		const auto& id = idIt->get_ref<const std::string&>();
		if (id.empty() || id.size() > kMaxRequestIdLength) {
			Surface(a_viewId, "invalid-request",
				std::format("request id must be 1-{} characters", kMaxRequestIdLength),
				{ { "name", name } });
			NoteTracedReply("invalid-request");
			finish();
			return;
		}
		DispatchRequest(name, id, payload);
		finish();
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
		if (const auto it = _compatCommands.find(a_name); it != _compatCommands.end()) {
			it->second(a_payload, *this);
			return;
		}
		// Kind enforcement: executing a mutation whose kind the caller got
		// wrong invites worse bugs, so the send is dropped. Dropping SILENTLY
		// is the part 1.x got wrong — surface it.
		if (_requests.contains(a_name)) {
			Surface(_currentSource, "wrong-endpoint-kind",
				std::format("'{}' is a request endpoint — use request(), not send()", a_name),
				{ { "name", a_name } });
			NoteTracedReply("wrong-endpoint-kind");
			return;
		}
		// Pages retry unregistered endpoints (polling), so warn once per name
		// to avoid flooding the log. The page-side surface still fires every
		// time — the page needs it.
		constexpr std::size_t kMaxWarnedEndpoints = 512;
		if (_warnedUnknownEndpoints.size() < kMaxWarnedEndpoints &&
			_warnedUnknownEndpoints.insert(a_name).second) {
			REX::WARN("MessageBridge: [content] dropped send to unknown endpoint '{}' "
					  "(further drops of this endpoint are not logged)", a_name);
		}
		Surface(_currentSource, "unknown-endpoint", "no such endpoint", { { "name", a_name } });
		NoteTracedReply("unknown-endpoint");
	}

	void MessageBridge::DispatchRequest(const std::string& a_name, const std::string& a_id, const nlohmann::json& a_payload)
	{
		_currentRequestId = a_id;

		const auto it = _requests.find(a_name);
		if (it == _requests.end()) {
			if (const auto compat = _compatCommands.find(a_name); compat != _compatCommands.end()) {
				compat->second(a_payload, *this);
				Respond(nlohmann::json{ { "ok", true }, { "command", a_name } });
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

		// Capacity is checked before dispatch so a saturated view cannot make
		// the host do the handler's work as well.
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
			// A request endpoint that neither settled nor deferred is a
			// platform bug, and the one failure mode the caller cannot
			// distinguish from a hang. Make it loud on both sides.
			REX::ERROR("MessageBridge: request endpoint '{}' returned without settling", a_name);
			Reject("internal", "the endpoint did not answer");
		}
	}

	void MessageBridge::HandleHello(std::string_view a_viewId)
	{
		// The queue is NOT cleared here. What it holds are the events emitted
		// between this view's creation and its document greeting us — which is
		// exactly the native ABI's message-before-first-paint guarantee
		// (RegisterView -> SendToWeb -> RequestMenu in one tick). Dropping them
		// as "stale from the previous document" would break that guarantee and
		// make the flush below dead code; a genuine re-greeting cannot have a
		// backlog anyway, because its gate was open and its events went
		// straight out. OnViewCreated is the only thing that discards a queue.
		_gates[std::string(a_viewId)].greeted = true;

		// 1. `ready`, before anything else this document will see.
		SendReady(a_viewId);
		// 2. every current state value: platform keys plus the owning mod's.
		//    PublishState only reaches a greeted view, which is why `greeted`
		//    is set above rather than here — the replay is the reason the gate
		//    exists at all.
		if (_onHello) {
			_onHello(a_viewId);
		}
		// 3. events resume, oldest first. `eventsOpen` is still false through
		//    the replay, so anything a replay listener raised appended to the
		//    queue behind the pre-greet backlog instead of overtaking it: the
		//    page sees ready, then all state, then every event in the order it
		//    actually happened.
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

	// -----------------------------------------------------------------------
	// settlement
	// -----------------------------------------------------------------------

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
			// A send endpoint reporting failure has no one to report to. Its
			// own WARN log records it; wanting an outcome means it should be a
			// request endpoint.
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

	void MessageBridge::Defer()
	{
		if (_currentRequestId.empty() || _settled) {
			REX::WARN("MessageBridge: Defer() outside an unsettled request ('{}')", _currentName);
			return;
		}
		_settled = true;
		_pending[_currentRequestId] = Pending{
			.view = _currentSource,
			.name = _currentName,
			.deadline = std::chrono::steady_clock::now() + kRequestDeadline,
		};
		NoteTracedReply("deferred");
	}

	void MessageBridge::RespondTo(std::string_view a_requestId, const nlohmann::json& a_payload)
	{
		RespondJsonTo(a_requestId, Json::Dump(a_payload));
	}

	void MessageBridge::RespondJsonTo(std::string_view a_requestId, std::string_view a_payloadJson)
	{
		const auto it = _pending.find(std::string(a_requestId));
		if (it == _pending.end()) {
			// Late or duplicate: the request already settled, expired, or went
			// away with its view. Never deliver twice.
			return;
		}
		const auto view = it->second.view;
		_pending.erase(it);
		if (_send && !view.empty()) {
			_send(view, EncodeReply(a_requestId, a_payloadJson));
		}
		NoteTracedReply("reply");
	}

	void MessageBridge::RejectTo(std::string_view a_requestId, std::string_view a_code, std::string_view a_message)
	{
		const auto it = _pending.find(std::string(a_requestId));
		if (it == _pending.end()) {
			return;
		}
		const auto view = it->second.view;
		_pending.erase(it);
		if (_send && !view.empty()) {
			_send(view, EncodeError(a_requestId, a_code, a_message));
		}
		NoteTracedReply(std::string("error:") + std::string(a_code));
	}

	// -----------------------------------------------------------------------
	// outbound pushes
	// -----------------------------------------------------------------------

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
		// A document that has not greeted the bridge gets every current value
		// through the hello replay, so there is nothing to queue here — and
		// queueing would risk delivering a stale value after a newer one.
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
		// Only greeted views: an event is a one-shot happening, and queueing it
		// for a document that has not asked for anything yet would deliver a
		// happening it could not have been present for.
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
		// `version` is the running plugin version — the reference point every
		// advisory `targetVersion` (view manifests, settings schemas) and any
		// view-side newer-host check compares against. `bridgeVersion` is the
		// protocol version, informational. `view`/`mod` tell the document who
		// it is, which is what makes a state key like "<mod>/<key>" writable
		// without the page hardcoding its own id.
		const nlohmann::json payload{
			{ "game", "Starfield" },
			{ "plugin", kPluginName },
			{ "version", kPluginVersion },
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

	// -----------------------------------------------------------------------
	// view lifecycle
	// -----------------------------------------------------------------------

	void MessageBridge::OnViewCreated(std::string_view a_viewId)
	{
		// Arm a CLOSED gate: everything emitted between here and the new
		// document's hello is queued rather than shouted at a page that has no
		// listener installed yet.
		auto& gate = _gates[std::string(a_viewId)];
		gate.greeted = false;
		gate.eventsOpen = false;
		gate.queued.clear();
	}

	void MessageBridge::OnViewDestroyed(std::string_view a_viewId)
	{
		_gates.erase(std::string(a_viewId));
		// Reap the view's deferred requests: nothing can be delivered to a page
		// that is gone, and leaving them pending would hold the view's capacity
		// for the process lifetime.
		for (auto it = _pending.begin(); it != _pending.end();) {
			if (it->second.view == a_viewId) {
				REX::DEBUG("MessageBridge: reaped in-flight request '{}' — view '{}' went away",
					it->second.name, a_viewId);
				it = _pending.erase(it);
			} else {
				++it;
			}
		}
	}

	bool MessageBridge::HasGreeted(std::string_view a_viewId) const
	{
		const auto it = _gates.find(std::string(a_viewId));
		return it != _gates.end() && it->second.greeted;
	}

	void MessageBridge::Tick(std::chrono::steady_clock::time_point a_now)
	{
		if (_pending.empty()) {
			return;
		}
		std::vector<std::pair<std::string, Pending>> expired;
		for (auto it = _pending.begin(); it != _pending.end();) {
			if (a_now >= it->second.deadline) {
				expired.emplace_back(it->first, it->second);
				it = _pending.erase(it);
			} else {
				++it;
			}
		}
		for (const auto& [id, req] : expired) {
			REX::WARN("MessageBridge: '{}' from view '{}' missed the {}s host deadline",
				req.name, req.view, std::chrono::duration_cast<std::chrono::seconds>(kRequestDeadline).count());
			if (_send && !req.view.empty()) {
				_send(req.view, EncodeError(id, "no-response", "the backend never answered"));
			}
			Surface(req.view, "no-response", "the backend never answered", { { "name", req.name } });
		}
	}

	// -----------------------------------------------------------------------
	// diagnostics
	// -----------------------------------------------------------------------

	void MessageBridge::Surface(std::string_view a_viewId, std::string_view a_code, std::string_view a_message,
		const nlohmann::json& a_detail)
	{
		REX::WARN("MessageBridge: [content] view '{}': {} — {}", a_viewId, a_code, a_message);
		if (_surface) {
			_surface(a_viewId, a_code, a_message, a_detail);
		}
	}

	void MessageBridge::NoteTracedReply(std::string_view a_what)
	{
		if (!_inMessage) {
			return;  // unsolicited push: not part of any message's trace
		}
		// Bounded: a handler answering in a loop must not grow this without
		// limit. Past the cap the line ends in an ellipsis rather than growing.
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

	// -----------------------------------------------------------------------
	// encoders
	//
	// The payload/value is already serialized, so it is spliced into the
	// envelope rather than deep-copied into a temporary json object. Key order
	// is fixed here (not nlohmann's) so the wire output is stable and readable
	// in a trace.
	// -----------------------------------------------------------------------

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
		// Handler messages are author text: bound on a codepoint boundary so a
		// split UTF-8 sequence can never reach dump().
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
