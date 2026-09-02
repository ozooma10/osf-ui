#include "API/BridgeApi.h"

#include "Bridge/MessageBridge.h"
#include "Core/Ids.h"
#include "Core/Json.h"

namespace OSFUI::API
{
	namespace
	{
		constexpr std::size_t kMaxPendingSendsPerView = 64;
		constexpr std::size_t kMaxInflightRequestsPerView = 64;

		const std::string* FindIdCaseInsensitive(
			const std::unordered_set<std::string>& a_ids, std::string_view a_wanted)
		{
			const auto found = std::ranges::find_if(a_ids, [a_wanted](const auto& id) {
				return Ids::EqualsCaseInsensitiveAscii(id, a_wanted);
			});
			return found == a_ids.end() ? nullptr : &*found;
		}
	}

	bool IsUnreservedEndpointName(std::string_view a_name)
	{
		if (a_name.empty() || a_name.size() > 128) return false;
		if (a_name.size() >= Ids::kBuiltInModId.size() &&
			Ids::EqualsCaseInsensitiveAscii(a_name.substr(0, Ids::kBuiltInModId.size()),
				Ids::kBuiltInModId) &&
			(a_name.size() == Ids::kBuiltInModId.size() ||
				a_name[Ids::kBuiltInModId.size()] == '.')) return false;
		static constexpr std::array kPlatformEndpoints{
			"close", "setVisible", "menu.open", "menu.close", "setViewHidden",
			"papyrus.call", "ping"
		};
		return std::ranges::find(kPlatformEndpoints, a_name) == kPlatformEndpoints.end();
	}

	BridgeApi& BridgeApi::Get()
	{
		static BridgeApi* const instance = new BridgeApi;
		return *instance;
	}

	bool BridgeApi::IsBridgeReady() { return _bridgeAvailable.load(); }

	void BridgeApi::RegisterSend(const char* a_name, Views::SendFn a_handler, void* a_user)
	{
		if (!a_name || !a_handler) return;
		const std::string name(a_name);
		if (!IsUnreservedEndpointName(name)) {
			REX::WARN("BridgeApi: refused RegisterSend('{}') — invalid or reserved", name.substr(0, 128));
			return;
		}
		std::lock_guard lock(_mutex);
		if (_sends.contains(name) || _requests.contains(name)) {
			REX::WARN("BridgeApi: refused RegisterSend('{}') — endpoint already registered", name);
			return;
		}
		_sends[name] = { a_handler, a_user };
		std::erase(_pendingSendUnregister, name);
		_dirty = true;
		MarkPending(kPendingPump);
	}

	void BridgeApi::UnregisterSend(const char* a_name)
	{
		if (!a_name) return;
		const std::string name(a_name);
		std::lock_guard lock(_mutex);
		if (_sends.erase(name)) {
			_pendingSendUnregister.push_back(name);
			_dirty = true;
			MarkPending(kPendingPump);
		}
	}

	void BridgeApi::RegisterRequest(const char* a_name, Views::RequestFn a_handler, void* a_user)
	{
		if (!a_name || !a_handler) return;
		const std::string name(a_name);
		if (!IsUnreservedEndpointName(name)) {
			REX::WARN("BridgeApi: refused RegisterRequest('{}') — invalid or reserved", name.substr(0, 128));
			return;
		}
		std::lock_guard lock(_mutex);
		if (_sends.contains(name) || _requests.contains(name)) {
			REX::WARN("BridgeApi: refused RegisterRequest('{}') — endpoint already registered", name);
			return;
		}
		_requests[name] = { a_handler, a_user };
		std::erase(_pendingRequestUnregister, name);
		_dirty = true;
		MarkPending(kPendingPump);
	}

	void BridgeApi::UnregisterRequest(const char* a_name)
	{
		if (!a_name) return;
		const std::string name(a_name);
		std::lock_guard lock(_mutex);
		if (_requests.erase(name)) {
			_pendingRequestUnregister.push_back(name);
			_dirty = true;
			MarkPending(kPendingPump);
		}
	}

	bool BridgeApi::RegisterRelativePointer(const char* a_viewId,
		Views::RelativePointerFn a_handler, void* a_user)
	{
		if (!a_viewId || !a_handler || !Ids::IsValidQualifiedViewId(a_viewId)) return false;
		std::lock_guard lock(_mutex);
		return _relativePointers.emplace(std::string(a_viewId),
			RelativePointerRegistration{ a_handler, a_user }).second;
	}

	void BridgeApi::UnregisterRelativePointer(const char* a_viewId)
	{
		if (!a_viewId) return;
		std::lock_guard lock(_mutex);
		_relativePointers.erase(a_viewId);
	}

	bool BridgeApi::HasRelativePointer(std::string_view a_viewId)
	{
		std::lock_guard lock(_mutex);
		return _relativePointers.contains(std::string(a_viewId));
	}

	bool BridgeApi::DispatchRelativePointer(std::string_view a_viewId,
		Views::RelativePointerPhase a_phase, float a_dx, float a_dy, float a_wheel)
	{
		RelativePointerRegistration registration;
		{
			std::lock_guard lock(_mutex);
			const auto found = _relativePointers.find(std::string(a_viewId));
			if (found == _relativePointers.end()) return false;
			registration = found->second;
		}
		const std::string view(a_viewId);
		registration.fn(view.c_str(), a_phase, a_dx, a_dy, a_wheel, registration.user);
		return true;
	}

	bool BridgeApi::RegisterViewOpenPreflight(const char* a_viewId,
		Views::ViewOpenPreflightFn a_handler, void* a_user)
	{
		if (!a_viewId || !a_handler || !Ids::IsValidQualifiedViewId(a_viewId)) return false;
		std::lock_guard lock(_mutex);
		return _viewOpenPreflights.emplace(std::string(a_viewId),
			ViewOpenPreflightRegistration{ a_handler, a_user }).second;
	}

	void BridgeApi::UnregisterViewOpenPreflight(const char* a_viewId)
	{
		if (!a_viewId) return;
		std::lock_guard lock(_mutex);
		_viewOpenPreflights.erase(a_viewId);
	}

	BridgeApi::ViewOpenPreflightResult BridgeApi::RunViewOpenPreflight(std::string_view a_viewId)
	{
		ViewOpenPreflightRegistration registration;
		{
			std::lock_guard lock(_mutex);
			const auto found = _viewOpenPreflights.find(std::string(a_viewId));
			if (found == _viewOpenPreflights.end()) return ViewOpenPreflightResult::kNoHandler;
			registration = found->second;
		}
		const std::string view(a_viewId);
		return registration.fn(view.c_str(), registration.user) ?
			ViewOpenPreflightResult::kAllowed : ViewOpenPreflightResult::kDenied;
	}

	bool BridgeApi::RegisterViewLifecycle(const char* a_viewId,
		Views::ViewLifecycleFn a_handler, void* a_user)
	{
		if (!a_viewId || !a_handler || !Ids::IsValidQualifiedViewId(a_viewId)) return false;
		std::lock_guard lock(_mutex);
		return _viewLifecycles.emplace(std::string(a_viewId),
			ViewLifecycleRegistration{ a_handler, a_user }).second;
	}

	void BridgeApi::UnregisterViewLifecycle(const char* a_viewId)
	{
		if (!a_viewId) return;
		std::lock_guard lock(_mutex);
		_viewLifecycles.erase(a_viewId);
	}

	bool BridgeApi::DispatchViewLifecycle(const std::string& a_viewId,
		Views::ViewLifecyclePhase a_phase)
	{
		ViewLifecycleRegistration registration;
		{
			std::lock_guard lock(_mutex);
			const auto found = _viewLifecycles.find(a_viewId);
			if (found == _viewLifecycles.end()) return false;
			registration = found->second;
		}
		registration.fn(a_viewId.c_str(), a_phase, registration.user);
		return true;
	}

	bool BridgeApi::SendToWeb(const char* a_viewId, const char* a_type,
		const char* a_payloadJson)
	{
		if (!a_viewId || !a_type || !a_type[0] || !a_payloadJson ||
			!Ids::IsValidQualifiedViewId(a_viewId) || !Json::Parse(a_payloadJson)) return false;
		std::lock_guard lock(_mutex);
		std::size_t count = 0;
		for (const auto& send : _pendingSends) count += send.view == a_viewId;
		if (count >= kMaxPendingSendsPerView) {
			const auto oldest = std::ranges::find_if(_pendingSends,
				[&](const PendingSend& send) { return send.view == a_viewId; });
			_pendingSends.erase(oldest);
		}
		_pendingSends.push_back({ a_viewId, a_type, a_payloadJson });
		MarkPending(kPendingPump);
		return true;
	}

	bool BridgeApi::SetViewState(const char* a_modId, const char* a_key,
		const char* a_payloadJson)
	{
		if (!a_modId || !a_key || !a_key[0] || !a_payloadJson ||
			!Ids::IsValidModId(a_modId) || std::string_view(a_key).size() > 128) return false;
		auto parsed = Json::Parse(a_payloadJson);
		if (!parsed) return false;
		std::lock_guard lock(_mutex);
		if (_pendingStateOps.size() >= 256) return false;
		_pendingStateOps.push_back({ a_modId, a_key, std::move(*parsed) });
		MarkPending(kPendingState);
		return true;
	}

	void BridgeApi::SetReadyCallback(Views::ReadyFn a_callback, void* a_user)
	{
		std::unique_lock lock(_mutex);
		if (_readyInvoking && _readyInvokingThread != std::this_thread::get_id()) {
			_readyInvokeCv.wait(lock, [this] { return !_readyInvoking; });
		}
		_readyCb = a_callback;
		_readyUser = a_user;
		if (_bridgeAvailable.load()) {
			_readyFired = false;
			MarkPending(kPendingPump);
		}
	}

	bool BridgeApi::RequestMenu(const char* a_viewId, bool a_open)
	{
		if (!a_viewId || !Ids::IsValidQualifiedViewId(a_viewId)) return false;
		std::lock_guard lock(_mutex);
		const auto* id = FindIdCaseInsensitive(
			a_open ? _knownViews : _instantiatedViews, a_viewId);
		if (!id) return false;
		_pendingViewPresentationRequests.push_back({ *id, a_open,
			std::chrono::steady_clock::now() });
		MarkPending(kPendingPresentation);
		return true;
	}

	void BridgeApi::SetViewCatalog(const std::vector<std::string>& a_viewIds)
	{
		std::lock_guard lock(_mutex);
		_knownViews.clear();
		_knownViews.insert(a_viewIds.begin(), a_viewIds.end());
		_instantiatedViews.clear();
		_viewCatalogReady = true;
		MarkPending(kPendingPump);
	}

	void BridgeApi::SetViewInstantiated(std::string_view a_viewId, bool a_instantiated)
	{
		std::lock_guard lock(_mutex);
		const auto* known = FindIdCaseInsensitive(_knownViews, a_viewId);
		const std::string id = known ? *known : std::string(a_viewId);
		if (a_instantiated) _instantiatedViews.emplace(id);
		else {
			if (const auto* found = FindIdCaseInsensitive(_instantiatedViews, id))
				_instantiatedViews.erase(*found);
			std::erase_if(_inflightRequests,
				[&](const auto& item) { return item.second.view == id; });
		}
		MarkPending(kPendingPump);
	}

	std::vector<BridgeApi::ViewPresentationRequest> BridgeApi::TakeViewPresentationRequests()
	{
		const auto reasons = _pending.fetch_and(~kPendingPresentation, std::memory_order_acq_rel);
		if (!(reasons & kPendingPresentation)) return {};
		std::lock_guard lock(_mutex);
		std::vector<ViewPresentationRequest> out;
		out.swap(_pendingViewPresentationRequests);
		return out;
	}

	bool BridgeApi::RegisterView(const char* a_viewId)
	{
		if (!a_viewId || !Ids::IsValidQualifiedViewId(a_viewId) ||
			!Ids::IsValidModId(Ids::ModOf(a_viewId))) return false;
		std::lock_guard lock(_mutex);
		_pendingViewRegs.emplace_back(a_viewId);
		MarkPending(kPendingViewRegistrations);
		return true;
	}

	std::vector<std::string> BridgeApi::TakeViewRegistrations()
	{
		const auto reasons = _pending.fetch_and(~kPendingViewRegistrations,
			std::memory_order_acq_rel);
		if (!(reasons & kPendingViewRegistrations)) return {};
		std::lock_guard lock(_mutex);
		std::vector<std::string> out;
		out.swap(_pendingViewRegs);
		return out;
	}

	std::vector<BridgeApi::ViewStateOp> BridgeApi::TakeViewStateOps()
	{
		const auto reasons = _pending.fetch_and(~kPendingState, std::memory_order_acq_rel);
		if (!(reasons & kPendingState)) return {};
		std::lock_guard lock(_mutex);
		std::vector<ViewStateOp> out;
		out.swap(_pendingStateOps);
		return out;
	}

	BridgeApi::PendingBatch BridgeApi::TakePendingBatch()
	{
		constexpr auto frameBits = kPendingPresentation | kPendingState |
			kPendingViewRegistrations;
		const auto reasons = _pending.fetch_and(~frameBits, std::memory_order_acq_rel);
		PendingBatch batch;
		if (!(reasons & frameBits)) return batch;
		std::lock_guard lock(_mutex);
		batch.presentation.swap(_pendingViewPresentationRequests);
		batch.state.swap(_pendingStateOps);
		batch.viewRegistrations.swap(_pendingViewRegs);
		return batch;
	}

	void BridgeApi::RespondThunk(std::uint64_t token, const char* type,
		const char* json) noexcept { Get().RespondRequest(token, type, json); }
	void BridgeApi::RejectThunk(std::uint64_t token, const char* code,
		const char* message) noexcept { Get().RejectRequest(token, code, message); }

	void BridgeApi::RespondRequest(std::uint64_t token, const char*, const char* json) noexcept
	{
		const auto parsed = json ? Json::Parse(json) : std::nullopt;
		std::lock_guard lock(_mutex);
		const auto found = _inflightRequests.find(token);
		if (found == _inflightRequests.end() || found->second.answered) return;
		found->second.answered = true;
		if (!parsed) {
			found->second.rejected = true;
			found->second.code = "invalid-response";
			found->second.message = "plugin returned invalid JSON";
		} else found->second.payloadJson = Json::Dump(*parsed);
		MarkPending(kPendingPump);
	}

	void BridgeApi::RejectRequest(std::uint64_t token, const char* code,
		const char* message) noexcept
	{
		std::lock_guard lock(_mutex);
		const auto found = _inflightRequests.find(token);
		if (found == _inflightRequests.end() || found->second.answered) return;
		found->second.answered = true;
		found->second.rejected = true;
		found->second.code = code && code[0] ? code : "plugin-error";
		found->second.message = message ? message : "";
		MarkPending(kPendingPump);
	}

	void BridgeApi::DropInflightRequest(std::uint64_t a_token) noexcept
	{
		std::lock_guard lock(_mutex);
		_inflightRequests.erase(a_token);
	}

	void BridgeApi::DispatchRequest(const std::string& a_name,
		const RequestRegistration& a_registration, const nlohmann::json& a_payload,
		MessageBridge& a_bridge)
	{
		const std::string view(a_bridge.CurrentSource());
		const std::string payload = Json::Dump(a_payload);
		std::uint64_t token;
		{
			std::lock_guard lock(_mutex);
			const auto count = std::ranges::count_if(_inflightRequests,
				[&](const auto& item) { return item.second.view == view; });
			if (count >= kMaxInflightRequestsPerView) {
				a_bridge.Reject("request-capacity", "too many requests are in flight");
				return;
			}
			token = _nextRequestToken++;
		}
		const std::string defer = a_bridge.Defer([this, token] { DropInflightRequest(token); });
		{
			std::lock_guard lock(_mutex);
			_inflightRequests.emplace(token,
				InflightRequest{ token, view, defer, a_name });
		}
		Views::Request request;
		request.name = a_name.c_str();
		request.payloadJson = payload.c_str();
		request.sourceViewId = view.c_str();
		request._token = token;
		request._respond = &RespondThunk;
		request._reject = &RejectThunk;
		a_registration.fn(request, a_registration.user);
	}

	void BridgeApi::SetBridgeAvailability(MessageBridge* a_bridge)
	{
		std::lock_guard lock(_mutex);
		_bridge = a_bridge;
		if (!a_bridge) _inflightRequests.clear();
		MarkPending(kPendingPump);
	}

	void BridgeApi::PumpMainThread()
	{
		const auto reasons = _pending.fetch_and(~kPendingPump, std::memory_order_acq_rel);
		if (!(reasons & kPendingPump)) return;
		MessageBridge* bridge = nullptr;
		std::vector<std::string> sendRemovals, requestRemovals;
		std::vector<std::pair<std::string, Registration>> sendsToRegister;
		std::vector<std::pair<std::string, RequestRegistration>> requestsToRegister;
		std::vector<PendingSend> sends;
		std::vector<PendingReply> replies;
		bool fireReady = false;
		Views::ReadyFn ready = nullptr;
		void* readyUser = nullptr;
		{
			std::lock_guard lock(_mutex);
			bridge = _bridge;
			if (bridge) {
				const bool changed = bridge != _appliedBridge;
				if (changed || _dirty) {
					if (changed) {
						_pendingSendUnregister.clear();
						_pendingRequestUnregister.clear();
					} else {
						sendRemovals.swap(_pendingSendUnregister);
						requestRemovals.swap(_pendingRequestUnregister);
					}
					for (const auto& item : _sends) sendsToRegister.push_back(item);
					for (const auto& item : _requests) requestsToRegister.push_back(item);
					_appliedBridge = bridge;
					_dirty = false;
				}
				for (auto it = _pendingSends.begin(); it != _pendingSends.end();) {
					if (_instantiatedViews.contains(it->view)) {
						sends.push_back(std::move(*it));
						it = _pendingSends.erase(it);
					} else if (_viewCatalogReady && !_knownViews.contains(it->view)) {
						it = _pendingSends.erase(it);
					} else ++it;
				}
				if (!_readyFired) {
					_readyFired = true;
					fireReady = true;
					ready = _readyCb;
					readyUser = _readyUser;
				}
				for (auto it = _inflightRequests.begin(); it != _inflightRequests.end();) {
					if (!it->second.answered) { ++it; continue; }
					auto& request = it->second;
					replies.push_back({ request.view, request.deferToken, request.name,
						request.payloadJson, request.rejected, request.code, request.message });
					it = _inflightRequests.erase(it);
				}
			}
		}
		if (bridge) {
			for (const auto& name : sendRemovals) bridge->UnregisterSend(name);
			for (const auto& name : requestRemovals) bridge->UnregisterRequest(name);
			for (const auto& [name, registration] : sendsToRegister) {
				bridge->RegisterSend(name, [name, registration](const nlohmann::json& payload,
					MessageBridge& source) {
					const auto json = Json::Dump(payload);
					const std::string view(source.CurrentSource());
					registration.fn(name.c_str(), json.c_str(), view.c_str(), registration.user);
				});
			}
			for (const auto& [name, registration] : requestsToRegister) {
				bridge->RegisterRequest(name, [this, name, registration](
					const nlohmann::json& payload, MessageBridge& source) {
					DispatchRequest(name, registration, payload, source);
				});
			}
			for (const auto& send : sends)
				bridge->EmitJson(send.view, send.type, send.payloadJson);
			for (const auto& reply : replies) {
				if (reply.rejected) bridge->RejectTo(reply.deferToken, reply.code, reply.message);
				else bridge->RespondJsonTo(reply.deferToken, reply.payloadJson);
			}
		}
		_bridgeAvailable.store(bridge != nullptr);
		bool invokeReady = false;
		if (fireReady && ready) {
			std::lock_guard lock(_mutex);
			if (_readyCb == ready && _readyUser == readyUser) {
				_readyInvoking = true;
				_readyInvokingThread = std::this_thread::get_id();
				invokeReady = true;
			}
		}
		if (invokeReady) {
			ready(readyUser);
			{
				std::lock_guard lock(_mutex);
				_readyInvoking = false;
				_readyInvokingThread = {};
			}
			_readyInvokeCv.notify_all();
		}
	}
}
