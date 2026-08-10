#include "compat/v1/LegacyEndpoints.h"

#include "api/BridgeApi.h"
#include "runtime/Ids.h"
#include "runtime/Json.h"

namespace OSFUI::Compat::V1
{
	namespace
	{
		// Mirrors the strict path's bounds (api/BridgeApi.cpp): the legacy
		// ledger is a sibling of _inflightRequests, so it gets the same
		// per-view cap and the same 30 s endpoint deadline.
		constexpr std::size_t kMaxInflightRequestsPerView = 64;
		constexpr auto kRequestTimeout = std::chrono::seconds(30);

		// One 1.x command invocation: the payload is re-serialized and handed
		// to the plugin's C callback with its source view id.
		void InvokeCommand(const char* a_fn_name, CommandFn a_fn, void* a_user,
			const nlohmann::json& a_payload, MessageBridge& a_bridge)
		{
			const auto dump = Json::Dump(a_payload);
			const std::string source(a_bridge.CurrentSource());
			a_fn(a_fn_name, dump.c_str(), source.c_str(), a_user);
		}
	}

	LegacyEndpoints& LegacyEndpoints::Get()
	{
		// Same lifetime rule as BridgeApi::Get(): plugin threads can still be
		// inside these mutexes when Windows begins DLL detach.
		static LegacyEndpoints* const instance = new LegacyEndpoints;
		return *instance;
	}

	void LegacyEndpoints::RegisterCommand(const char* a_name, CommandFn a_fn, void* a_user)
	{
		if (!a_name || !a_fn) return;
		const std::string name(a_name);
		if (!Ids::IsValidPluginEndpointName(name)) {
			REX::WARN("Compat::V1: [content] refused legacy RegisterCommand('{}') — commands are '<author>.<modname>.<name>'",
				name.substr(0, 128));
			return;
		}
		// Reserve before inserting: BridgeApi's set is what keeps strict and
		// legacy names disjoint, and it is never called under our lock.
		if (!API::BridgeApi::Get().TryReserveEndpointName(name.c_str())) {
			REX::WARN("Compat::V1: [content] refused legacy RegisterCommand('{}') — already registered (first wins)", name);
			return;
		}
		std::lock_guard lock(_mutex);
		_commands[name] = { a_fn, a_user };
	}

	void LegacyEndpoints::UnregisterCommand(const char* a_name)
	{
		if (!a_name) return;
		const std::string name(a_name);
		{
			std::lock_guard lock(_mutex);
			if (_commands.erase(name) == 0) return;
		}
		API::BridgeApi::Get().ReleaseEndpointName(name.c_str());
	}

	void LegacyEndpoints::RegisterRequest(const char* a_name, RequestFn a_fn, void* a_user)
	{
		if (!a_name || !a_fn) return;
		const std::string name(a_name);
		if (!Ids::IsValidPluginEndpointName(name)) {
			REX::WARN("Compat::V1: [content] refused legacy RegisterRequest('{}') — requests are '<author>.<modname>.<name>'",
				name.substr(0, 128));
			return;
		}
		if (!API::BridgeApi::Get().TryReserveEndpointName(name.c_str())) {
			REX::WARN("Compat::V1: [content] refused legacy RegisterRequest('{}') — already registered (first wins)", name);
			return;
		}
		std::lock_guard lock(_mutex);
		_requests[name] = { a_fn, a_user };
	}

	void LegacyEndpoints::UnregisterRequest(const char* a_name)
	{
		if (!a_name) return;
		const std::string name(a_name);
		{
			std::lock_guard lock(_mutex);
			if (_requests.erase(name) == 0) return;
		}
		API::BridgeApi::Get().ReleaseEndpointName(name.c_str());
	}

	LegacyEndpoints::SendOutcome LegacyEndpoints::TryDispatchSend(const std::string& a_name,
		const nlohmann::json& a_payload, MessageBridge& a_bridge)
	{
		Command cmd{};
		{
			std::lock_guard lock(_mutex);
			if (const auto it = _commands.find(a_name); it != _commands.end()) {
				cmd = it->second;
			} else {
				return _requests.contains(a_name) ? SendOutcome::kIsRequestEndpoint :
				                                    SendOutcome::kNotHandled;
			}
		}
		InvokeCommand(a_name.c_str(), cmd.fn, cmd.user, a_payload, a_bridge);
		return SendOutcome::kHandled;
	}

	bool LegacyEndpoints::TryDispatchRequest(const std::string& a_name, const std::string& a_id,
		const nlohmann::json& a_payload, MessageBridge& a_bridge)
	{
		Command cmd{};
		RequestReg req{};
		bool isCommand = false, isRequest = false;
		{
			std::lock_guard lock(_mutex);
			if (const auto it = _commands.find(a_name); it != _commands.end()) {
				cmd = it->second;
				isCommand = true;
			} else if (const auto it = _requests.find(a_name); it != _requests.end()) {
				req = it->second;
				isRequest = true;
			}
		}
		if (isCommand) {
			// The 1.x wire carried the correlation id inside the payload, and a
			// command that produced no reply of its own got a uniform ack.
			auto payload = a_payload;
			payload["requestId"] = a_id;
			InvokeCommand(a_name.c_str(), cmd.fn, cmd.user, payload, a_bridge);
			if (!a_bridge.CurrentSettled()) {
				a_bridge.Respond(nlohmann::json{ { "ok", true }, { "command", a_name } });
			}
			return true;
		}
		if (isRequest) {
			DispatchRequest(a_name, req, a_payload, a_bridge);
			return true;
		}
		if (a_bridge.IsLegacyApiView(a_bridge.CurrentSource())) {
			// A 1.x request could correlate any ui.command, including a command
			// which is a strict send in 2.0. Keep that behavior only for
			// explicitly legacy documents; the same handler and source
			// authority checks still run, followed by the old uniform ack when
			// the handler produced no reply of its own.
			if (a_bridge.TryInvokeSendHandler(a_name, a_payload)) {
				if (!a_bridge.CurrentSettled()) {
					a_bridge.Respond(nlohmann::json{ { "ok", true }, { "command", a_name } });
				}
				return true;
			}
		}
		return false;
	}

	void LegacyEndpoints::DispatchRequest(const std::string& a_name, const RequestReg& a_reg,
		const nlohmann::json& a_payload, MessageBridge& a_bridge)
	{
		const std::string view(a_bridge.CurrentSource());
		{
			std::lock_guard lock(_mutex);
			const auto count = static_cast<std::size_t>(std::ranges::count_if(
				_inflight, [&](const auto& e) { return e.second.view == view; }));
			if (count >= kMaxInflightRequestsPerView) {
				a_bridge.Reject("request-capacity", "too many requests are already in flight");
				return;
			}
		}
		// Whether to wrap the reply is decided NOW, on the main thread, while
		// the source's gate still says what API the document speaks — the
		// plugin answers later from any thread.
		const bool wrap = a_bridge.IsLegacyApiView(view);
		// Take ownership of the correlation id before the plugin can answer;
		// Pump settles it (or expires it at the OSF UI runtime deadline).
		const std::string deferToken = a_bridge.Defer();
		if (deferToken.empty()) {
			return;
		}
		const std::string payloadJson = Json::Dump(a_payload);
		Request request;
		{
			std::lock_guard lock(_mutex);
			if (_ledgerBridge != &a_bridge) {
				// Defer tokens are minted per bridge instance; entries from a
				// previous instance can never settle in this one.
				_inflight.clear();
				_ledgerBridge = &a_bridge;
			}
			const auto token = _nextToken++;
			_inflight.emplace(token, Inflight{
									.view = view,
									.deferToken = deferToken,
									.name = a_name,
									.wrapReply = wrap,
									.deadline = std::chrono::steady_clock::now() + kRequestTimeout,
								});
			request._token = token;
		}
		request.command = a_name.c_str();
		request.payloadJson = payloadJson.c_str();
		request.sourceViewId = view.c_str();
		request._respond = &RespondThunk;
		request._reject = &RejectThunk;
		a_reg.fn(request, a_reg.user);
	}

	void LegacyEndpoints::RespondThunk(std::uint64_t a_token, const char* a_type, const char* a_json) noexcept
	{
		Get().RespondRequest(a_token, a_type, a_json);
	}

	void LegacyEndpoints::RejectThunk(std::uint64_t a_token, const char* a_code, const char* a_message) noexcept
	{
		Get().RejectRequest(a_token, a_code, a_message);
	}

	void LegacyEndpoints::RespondRequest(std::uint64_t a_token, const char* a_type, const char* a_json) noexcept
	{
		const auto parsed = a_json ? Json::Parse(a_json) : std::nullopt;
		std::lock_guard lock(_mutex);
		const auto it = _inflight.find(a_token);
		if (it == _inflight.end()) { REX::WARN("Compat::V1: ignored late response for stale token {}", a_token); return; }
		if (it->second.answered) { REX::WARN("Compat::V1: ignored second response for request '{}'", it->second.name); return; }
		it->second.answered = true;
		if (!parsed) {
			it->second.rejected = true;
			it->second.code = "invalid-response";
			it->second.message = "plugin returned invalid JSON";
			return;
		}
		it->second.type = (a_type && a_type[0]) ? a_type : it->second.name;
		it->second.payloadJson = Json::Dump(*parsed);
	}

	void LegacyEndpoints::RejectRequest(std::uint64_t a_token, const char* a_code, const char* a_message) noexcept
	{
		std::lock_guard lock(_mutex);
		const auto it = _inflight.find(a_token);
		if (it == _inflight.end()) { REX::WARN("Compat::V1: ignored late rejection for stale token {}", a_token); return; }
		if (it->second.answered) { REX::WARN("Compat::V1: ignored second response for request '{}'", it->second.name); return; }
		it->second.answered = true;
		it->second.rejected = true;
		it->second.code = (a_code && a_code[0]) ? a_code : "plugin-error";
		it->second.message = a_message ? a_message : "";
	}

	void LegacyEndpoints::Pump(MessageBridge& a_bridge)
	{
		std::vector<Inflight> done;
		{
			std::lock_guard lock(_mutex);
			if (&a_bridge != _ledgerBridge) {
				_inflight.clear();
				_ledgerBridge = &a_bridge;
			}
			const auto now = std::chrono::steady_clock::now();
			for (auto it = _inflight.begin(); it != _inflight.end();) {
				auto& req = it->second;
				if (!req.answered && now < req.deadline) {
					++it;
					continue;
				}
				if (!req.answered) {
					req.rejected = true;
					req.code = "no-response";
					req.message = "the plugin never answered";
				}
				done.push_back(std::move(req));
				it = _inflight.erase(it);
			}
		}
		for (const auto& reply : done) {
			if (reply.rejected) {
				REX::WARN("Compat::V1: request '{}' from view '{}' -> {}", reply.name, reply.view, reply.code);
				a_bridge.RejectTo(reply.deferToken, reply.code, reply.message);
			} else if (reply.wrapReply) {
				auto payload = Json::Parse(reply.payloadJson).value_or(nlohmann::json::object());
				a_bridge.RespondTo(reply.deferToken, {
											 { "__osfuiV1Reply", {
																	 { "type", reply.type },
																	 { "payload", std::move(payload) },
																 } },
										 });
			} else {
				a_bridge.RespondJsonTo(reply.deferToken, reply.payloadJson);
			}
		}
	}
}
