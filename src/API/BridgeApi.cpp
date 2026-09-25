#include "API/BridgeApi.h"

#include "Bridge/MessageBridge.h"
#include "Core/Ids.h"
#include "Core/Json.h"

namespace OSFUI::API
{
	namespace
	{
		constexpr std::size_t kMaxPendingSendsPerView = 64;

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

	bool BridgeApi::IsReady() noexcept { return m_bridgeAvailable.load(); }

	bool BridgeApi::RegisterSend(const char* a_name, SendFn a_handler, void* a_user) noexcept
	{
		if (!a_name || !a_handler) return false;
		const std::string name(a_name);
		if (!IsUnreservedEndpointName(name)) {
			REX::WARN("BridgeApi: refused RegisterSend('{}') — invalid or reserved", name.substr(0, 128));
			return false;
		}
		std::lock_guard lock(m_mutex);
		if (m_sends.contains(name) || m_requests.contains(name) || m_papyrusEndpoints.contains(StringUtil::ToLowerAscii(name))) {
			REX::WARN("BridgeApi: refused RegisterSend('{}') — endpoint already registered", name);
			return false;
		}
		m_sends[name] = { a_handler, a_user };
		m_dirty = true;
		MarkPending(kPendingPump);
		return true;
	}

	bool BridgeApi::RegisterRequest(const char* a_name, RequestFn a_handler, void* a_user) noexcept
	{
		if (!a_name || !a_handler) return false;
		const std::string name(a_name);
		if (!IsUnreservedEndpointName(name)) {
			REX::WARN("BridgeApi: refused RegisterRequest('{}') — invalid or reserved", name.substr(0, 128));
			return false;
		}
		std::lock_guard lock(m_mutex);
		if (m_sends.contains(name) || m_requests.contains(name) || m_papyrusEndpoints.contains(StringUtil::ToLowerAscii(name))) {
			REX::WARN("BridgeApi: refused RegisterRequest('{}') — endpoint already registered", name);
			return false;
		}
		m_requests[name] = { a_handler, a_user };
		m_dirty = true;
		MarkPending(kPendingPump);
		return true;
	}

	bool BridgeApi::RegisterRelativePointer(const char* a_viewId,
		RelativePointerFn a_handler, void* a_user) noexcept
	{
		if (!a_viewId || !a_handler || !Ids::IsValidQualifiedViewId(a_viewId)) return false;
		std::lock_guard lock(m_mutex);
		return m_relativePointers.emplace(StringUtil::ToLowerAscii(a_viewId),
			RelativePointerRegistration{ a_handler, a_user }).second;
	}

	void BridgeApi::UnregisterRelativePointer(const char* a_viewId) noexcept
	{
		if (!a_viewId) return;
		std::lock_guard dispatchLock(m_callbackDispatchMutex);
		std::lock_guard lock(m_mutex);
		m_relativePointers.erase(StringUtil::ToLowerAscii(a_viewId));
	}

	bool BridgeApi::HasRelativePointer(std::string_view a_viewId)
	{
		std::lock_guard lock(m_mutex);
		return m_relativePointers.contains(StringUtil::ToLowerAscii(a_viewId));
	}

	bool BridgeApi::DispatchRelativePointer(std::string_view a_viewId,
		RelativePointerPhase a_phase, float a_dx, float a_dy, float a_wheel)
	{
		std::lock_guard dispatchLock(m_callbackDispatchMutex);
		RelativePointerRegistration registration;
		{
			std::lock_guard lock(m_mutex);
			const auto found = m_relativePointers.find(StringUtil::ToLowerAscii(a_viewId));
			if (found == m_relativePointers.end()) return false;
			registration = found->second;
		}
		const std::string view(a_viewId);
		registration.fn(view.c_str(), a_phase, a_dx, a_dy, a_wheel, registration.user);
		return true;
	}

	bool BridgeApi::RegisterViewOpenPreflight(const char* a_viewId,
		ViewOpenPreflightFn a_handler, void* a_user) noexcept
	{
		if (!a_viewId || !a_handler || !Ids::IsValidQualifiedViewId(a_viewId)) return false;
		std::lock_guard lock(m_mutex);
		return m_viewOpenPreflights.emplace(StringUtil::ToLowerAscii(a_viewId),
			ViewOpenPreflightRegistration{ a_handler, a_user }).second;
	}

	void BridgeApi::UnregisterViewOpenPreflight(const char* a_viewId) noexcept
	{
		if (!a_viewId) return;
		std::lock_guard dispatchLock(m_callbackDispatchMutex);
		std::lock_guard lock(m_mutex);
		m_viewOpenPreflights.erase(StringUtil::ToLowerAscii(a_viewId));
	}

	BridgeApi::ViewOpenPreflightResult BridgeApi::RunViewOpenPreflight(std::string_view a_viewId)
	{
		std::lock_guard dispatchLock(m_callbackDispatchMutex);
		ViewOpenPreflightRegistration registration;
		{
			std::lock_guard lock(m_mutex);
			const auto found = m_viewOpenPreflights.find(StringUtil::ToLowerAscii(a_viewId));
			if (found == m_viewOpenPreflights.end()) return ViewOpenPreflightResult::kNoHandler;
			registration = found->second;
		}
		const std::string view(a_viewId);
		return registration.fn(view.c_str(), registration.user) ?
			ViewOpenPreflightResult::kAllowed : ViewOpenPreflightResult::kDenied;
	}

	bool BridgeApi::RegisterViewLifecycle(const char* a_viewId,
		ViewLifecycleFn a_handler, void* a_user) noexcept
	{
		if (!a_viewId || !a_handler || !Ids::IsValidQualifiedViewId(a_viewId)) return false;
		std::lock_guard lock(m_mutex);
		return m_viewLifecycles.emplace(StringUtil::ToLowerAscii(a_viewId),
			ViewLifecycleRegistration{ a_handler, a_user }).second;
	}

	void BridgeApi::UnregisterViewLifecycle(const char* a_viewId) noexcept
	{
		if (!a_viewId) return;
		std::lock_guard dispatchLock(m_callbackDispatchMutex);
		std::lock_guard lock(m_mutex);
		m_viewLifecycles.erase(StringUtil::ToLowerAscii(a_viewId));
	}

	bool BridgeApi::DispatchViewLifecycle(const std::string& a_viewId,
		ViewLifecyclePhase a_phase)
	{
		std::lock_guard dispatchLock(m_callbackDispatchMutex);
		ViewLifecycleRegistration registration;
		{
			std::lock_guard lock(m_mutex);
			const auto found = m_viewLifecycles.find(StringUtil::ToLowerAscii(a_viewId));
			if (found == m_viewLifecycles.end()) return false;
			registration = found->second;
		}
		registration.fn(a_viewId.c_str(), a_phase, registration.user);
		return true;
	}

	bool BridgeApi::SendToWeb(const char* a_viewId, const char* a_type,
		const char* a_payloadJson) noexcept
	{
		if (!a_viewId || !a_type || !a_type[0] || !a_payloadJson ||
			!Ids::IsValidQualifiedViewId(a_viewId) || std::string_view(a_type).size() > 128 ||
			std::string_view(a_payloadJson).size() > 1024u * 1024u) return false;
		auto parsed = Json::Parse(a_payloadJson);
		if (!parsed) return false;
		auto canonicalPayload = Json::Dump(*parsed);
		std::lock_guard lock(m_mutex);
		std::size_t count = 0;
		for (const auto& send : m_pendingSends) count += Ids::EqualsCaseInsensitiveAscii(send.view, a_viewId);
		if (count >= kMaxPendingSendsPerView) {
			const auto oldest = std::ranges::find_if(m_pendingSends,
				[&](const PendingSend& send) { return Ids::EqualsCaseInsensitiveAscii(send.view, a_viewId); });
			m_pendingSends.erase(oldest);
		}
		m_pendingSends.push_back({ a_viewId, a_type, std::move(canonicalPayload) });
		MarkPending(kPendingPump);
		return true;
	}

	bool BridgeApi::SetViewState(const char* a_modId, const char* a_key,
		const char* a_payloadJson) noexcept
	{
		if (!a_modId || !a_key || !a_key[0] || !a_payloadJson ||
			!Ids::IsValidModId(a_modId) || std::string_view(a_key).size() > 128) return false;
		auto parsed = Json::Parse(a_payloadJson);
		if (!parsed) return false;
		std::lock_guard lock(m_mutex);
		if (m_pendingStateOps.size() >= 256) return false;
		m_pendingStateOps.push_back({ a_modId, a_key, std::move(*parsed) });
		MarkPending(kPendingState);
		return true;
	}

	void BridgeApi::SetReadyCallback(ReadyFn a_callback, void* a_user) noexcept
	{
		std::unique_lock lock(m_mutex);
		if (m_readyInvoking && m_readyInvokingThread != std::this_thread::get_id()) {
			m_readyInvokeCv.wait(lock, [this] { return !m_readyInvoking; });
		}
		++m_readyRevision;
		m_readyCb = a_callback;
		m_readyUser = a_user;
		m_readyFired = false;
		MarkPending(kPendingPump);
	}

	bool BridgeApi::RequestMenu(const char* a_viewId, bool a_open) noexcept
	{
		if (!a_viewId || !Ids::IsValidQualifiedViewId(a_viewId)) return false;
		std::lock_guard lock(m_mutex);
		const auto* id = FindIdCaseInsensitive(
			a_open ? m_knownViews : m_instantiatedViews, a_viewId);
		if (!id) return false;
		m_pendingViewPresentationRequests.push_back({ *id, a_open,
			std::chrono::steady_clock::now() });
		MarkPending(kPendingPresentation);
		return true;
	}

	void BridgeApi::SetViewCatalog(const std::vector<std::string>& a_viewIds)
	{
		std::lock_guard lock(m_mutex);
		m_knownViews.clear();
		m_knownViews.insert(a_viewIds.begin(), a_viewIds.end());
		m_instantiatedViews.clear();
		m_viewCatalogReady = true;
		MarkPending(kPendingPump);
	}

	void BridgeApi::SetViewInstantiated(std::string_view a_viewId, bool a_instantiated)
	{
		std::lock_guard lock(m_mutex);
		const auto* known = FindIdCaseInsensitive(m_knownViews, a_viewId);
		const std::string id = known ? *known : std::string(a_viewId);
		if (a_instantiated) {
			m_instantiatedViews.emplace(id);
			m_readyFired = false; // includes replacing a document under an existing view ID
			++m_readyRevision;
		}
		else {
			if (const auto* found = FindIdCaseInsensitive(m_instantiatedViews, id))
				m_instantiatedViews.erase(*found);
			std::erase_if(m_inflightRequests,
				[&](const auto& item) { return item.second.view == id; });
		}
		MarkPending(kPendingPump);
	}

	bool BridgeApi::RegisterView(const char* a_viewId) noexcept
	{
		if (!a_viewId || !Ids::IsValidQualifiedViewId(a_viewId) ||
			!Ids::IsValidModId(Ids::ModOf(a_viewId))) return false;
		std::lock_guard lock(m_mutex);
		m_pendingViewRegs.emplace_back(a_viewId);
		MarkPending(kPendingViewRegistrations);
		return true;
	}

	BridgeApi::PendingBatch BridgeApi::TakePendingBatch()
	{
		constexpr auto frameBits = kPendingPresentation | kPendingState |
			kPendingViewRegistrations;
		const auto reasons = m_pending.fetch_and(~frameBits, std::memory_order_acq_rel);
		PendingBatch batch;
		if (!(reasons & frameBits)) return batch;
		std::lock_guard lock(m_mutex);
		batch.presentation.swap(m_pendingViewPresentationRequests);
		batch.state.swap(m_pendingStateOps);
		batch.viewRegistrations.swap(m_pendingViewRegs);
		return batch;
	}

	void BridgeApi::RespondThunk(std::uint64_t token, const char* json) noexcept
	{
		Get().RespondRequest(token, json);
	}
	void BridgeApi::RejectThunk(std::uint64_t token, const char* code,
		const char* message) noexcept { Get().RejectRequest(token, code, message); }

	void BridgeApi::RespondRequest(std::uint64_t token, const char* json) noexcept
	{
		const auto parsed = json ? Json::Parse(json) : std::nullopt;
		std::lock_guard lock(m_mutex);
		const auto found = m_inflightRequests.find(token);
		if (found == m_inflightRequests.end() || found->second.answered) return;
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
		std::lock_guard lock(m_mutex);
		const auto found = m_inflightRequests.find(token);
		if (found == m_inflightRequests.end() || found->second.answered) return;
		found->second.answered = true;
		found->second.rejected = true;
		found->second.code = code && code[0] ? code : "plugin-error";
		found->second.message = message ? message : "";
		MarkPending(kPendingPump);
	}

	void BridgeApi::DropInflightRequest(std::uint64_t a_token) noexcept
	{
		std::lock_guard lock(m_mutex);
		m_inflightRequests.erase(a_token);
	}

	void BridgeApi::DispatchRequest(const std::string& a_name,
		const RequestRegistration& a_registration, const nlohmann::json& a_payload,
		MessageBridge& a_bridge)
	{
		const std::string view(a_bridge.CurrentSource());
		const std::string payload = Json::Dump(a_payload);
		std::uint64_t token;
		{
			std::lock_guard lock(m_mutex);
			token = m_nextRequestToken++;
		}
		const std::string defer = a_bridge.Defer([this, token] { DropInflightRequest(token); });
		{
			std::lock_guard lock(m_mutex);
			m_inflightRequests.emplace(token,
				InflightRequest{ .view = view, .deferToken = defer });
		}
		Request request;
		request.name = a_name.c_str();
		request.payloadJson = payload.c_str();
		request.sourceViewId = view.c_str();
		request.m_token = token;
		request.m_respond = &RespondThunk;
		request.m_reject = &RejectThunk;
		a_registration.fn(request, a_registration.user);
	}

	bool BridgeApi::ClaimPapyrusEndpoint(std::string_view a_name)
	{
		std::lock_guard lock(m_mutex);
		const auto conflicts = [&](const auto& item) { return Ids::EqualsCaseInsensitiveAscii(item.first, a_name); };
		if (std::ranges::any_of(m_sends, conflicts) || std::ranges::any_of(m_requests, conflicts)) return false;
		return m_papyrusEndpoints.emplace(StringUtil::ToLowerAscii(a_name)).second;
	}

	void BridgeApi::ReleasePapyrusEndpoint(std::string_view a_name)
	{
		std::lock_guard lock(m_mutex);
		m_papyrusEndpoints.erase(StringUtil::ToLowerAscii(a_name));
	}

	void BridgeApi::SetBridgeAvailability(MessageBridge* a_bridge)
	{
		std::lock_guard lock(m_mutex);
		if (m_bridge != a_bridge) {
			m_readyFired = false;
			++m_readyRevision;
		}
		if (!a_bridge) m_appliedBridge = nullptr;
		m_bridge = a_bridge;
		m_bridgeAvailable.store(a_bridge != nullptr, std::memory_order_release);
		if (!a_bridge) m_inflightRequests.clear();
		MarkPending(kPendingPump);
	}

	void BridgeApi::PumpMainThread()
	{
		const auto reasons = m_pending.fetch_and(~kPendingPump, std::memory_order_acq_rel);
		if (!(reasons & kPendingPump)) return;
		MessageBridge* bridge = nullptr;
		std::vector<std::pair<std::string, Registration>> sendsToRegister;
		std::vector<std::pair<std::string, RequestRegistration>> requestsToRegister;
		std::vector<PendingSend> sends;
		std::vector<PendingReply> replies;
		bool fireReady = false;
		std::uint64_t readyRevision = 0;
		ReadyFn ready = nullptr;
		void* readyUser = nullptr;
		{
			std::lock_guard lock(m_mutex);
			bridge = m_bridge;
			if (bridge) {
				// Endpoints only ever grow, so a resync re-applies the whole set.
				if (bridge != m_appliedBridge || m_dirty) {
					for (const auto& item : m_sends) sendsToRegister.push_back(item);
					for (const auto& item : m_requests) requestsToRegister.push_back(item);
					m_appliedBridge = bridge;
					m_dirty = false;
				}
				for (auto it = m_pendingSends.begin(); it != m_pendingSends.end();) {
					if (const auto* canonical = FindIdCaseInsensitive(m_instantiatedViews, it->view)) {
						it->view = *canonical;
						sends.push_back(std::move(*it));
						it = m_pendingSends.erase(it);
					} else if (m_viewCatalogReady && !FindIdCaseInsensitive(m_knownViews, it->view)) {
						it = m_pendingSends.erase(it);
					} else ++it;
				}
				if (!m_readyFired) {
					m_readyFired = true;
					fireReady = true;
					readyRevision = m_readyRevision;
					ready = m_readyCb;
					readyUser = m_readyUser;
				}
				for (auto it = m_inflightRequests.begin(); it != m_inflightRequests.end();) {
					if (!it->second.answered) { ++it; continue; }
					auto& request = it->second;
					replies.push_back({ request.deferToken, request.payloadJson,
						request.rejected, request.code, request.message });
					it = m_inflightRequests.erase(it);
				}
			}
		}
		if (bridge) {
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
		bool invokeReady = false;
		if (fireReady && ready) {
			std::lock_guard lock(m_mutex);
			if (m_readyRevision == readyRevision && m_readyCb == ready && m_readyUser == readyUser) {
				m_readyInvoking = true;
				m_readyInvokingThread = std::this_thread::get_id();
				invokeReady = true;
			}
		}
		if (invokeReady) {
			ready(readyUser);
			{
				std::lock_guard lock(m_mutex);
				m_readyInvoking = false;
				m_readyInvokingThread = {};
			}
			m_readyInvokeCv.notify_all();
		}
	}
}
