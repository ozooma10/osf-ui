#include "API/BridgeApi.h"

#include <cassert>

#include "Bridge/MessageBridge.h"
#include "Core/Ids.h"
#include "Core/Json.h"

namespace OSFUI::API
{
	namespace
	{
		constexpr std::size_t kMaxPendingSendsPerView = 64;
		constexpr std::size_t kMaxQueuedReplies = 256;

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
		std::lock_guard dispatchLock(m_callbackDispatchMutex);
		std::lock_guard lock(m_mutex);
		m_readyCb = a_callback;
		m_readyUser = a_user;
		m_readyFired = false;
		MarkPending(kPendingPump);
	}

	bool BridgeApi::RequestMenu(const char* a_viewId, bool a_open) noexcept
	{
		if (!a_viewId || !Ids::IsValidQualifiedViewId(a_viewId)) return false;
		std::lock_guard lock(m_mutex);
		const auto* id = FindIdCaseInsensitive(m_knownViews, a_viewId);
		if (!id) return false;
		m_viewRequests.EnqueueView(*id, a_open);
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
		}
		else {
			if (const auto* found = FindIdCaseInsensitive(m_instantiatedViews, id)) {
				m_instantiatedViews.erase(*found);
			}
		}
		MarkPending(kPendingPump);
	}

	bool BridgeApi::RegisterView(const char* a_viewId) noexcept
	{
		if (!a_viewId || !Ids::IsValidQualifiedViewId(a_viewId) || !Ids::IsValidModId(Ids::ModOf(a_viewId))) return false;
		std::lock_guard lock(m_mutex);
		m_pendingViewRegs.emplace_back(a_viewId);
		MarkPending(kPendingViewRegistrations);
		return true;
	}

	BridgeApi::PendingBatch BridgeApi::TakePendingBatch()
	{
		PendingBatch batch;
		batch.presentation = m_viewRequests.Take();
		constexpr auto frameBits = kPendingState | kPendingViewRegistrations;
		const auto reasons = m_pending.fetch_and(~frameBits, std::memory_order_acq_rel);
		if (!(reasons & frameBits)) return batch;
		std::lock_guard lock(m_mutex);
		batch.state.swap(m_pendingStateOps);
		batch.viewRegistrations.swap(m_pendingViewRegs);
		return batch;
	}

	std::vector<BridgeApi::ViewStateOp> BridgeApi::TakePendingState()
	{
		const auto reasons = m_pending.fetch_and(~kPendingState, std::memory_order_acq_rel);
		std::vector<ViewStateOp> state;
		if (!(reasons & kPendingState)) return state;
		std::lock_guard lock(m_mutex);
		state.swap(m_pendingStateOps);
		return state;
	}

	void BridgeApi::RespondThunk(std::uint64_t token, const char* json) noexcept
	{
		Get().RespondRequest(token, json);
	}
	void BridgeApi::RejectThunk(std::uint64_t token, const char* code,
		const char* message) noexcept { Get().RejectRequest(token, code, message); }

	// Any thread. Late, duplicate and stale answers are filtered by the bridge on the main thread.
	void BridgeApi::RespondRequest(std::uint64_t token, const char* json) noexcept
	{
		const auto parsed = json ? Json::Parse(json) : std::nullopt;
		if (!parsed) {
			QueueReply({ .token = token, .rejected = true, .code = "invalid-response",
				.message = "plugin returned invalid JSON" });
		} else {
			QueueReply({ .token = token, .payloadJson = Json::Dump(*parsed) });
		}
	}

	void BridgeApi::RejectRequest(std::uint64_t token, const char* code,
		const char* message) noexcept
	{
		QueueReply({ .token = token, .rejected = true,
			.code = code && code[0] ? code : "plugin-error", .message = message ? message : "" });
	}

	void BridgeApi::QueueReply(QueuedReply a_reply) noexcept
	{
		if (a_reply.token == 0) return;
		std::lock_guard lock(m_mutex);
		if (m_queuedReplies.size() >= kMaxQueuedReplies) {
			REX::WARN("BridgeApi: dropped a plugin reply; {} replies already wait for the game thread", kMaxQueuedReplies);
			return;
		}
		m_queuedReplies.push_back(std::move(a_reply));
		MarkPending(kPendingPump);
	}

	void BridgeApi::DispatchRequest(const std::string& a_name,
		const RequestRegistration& a_registration, const nlohmann::json& a_payload,
		MessageBridge& a_bridge)
	{
		const std::string view(a_bridge.CurrentSource());
		const std::string payload = Json::Dump(a_payload);
		const auto token = a_bridge.Defer();
		if (token == 0) return;
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

	void BridgeApi::AttachBridge(MessageBridge& a_bridge)
	{
		std::lock_guard lock(m_mutex);
		assert(!m_bridge);
		m_bridge = &a_bridge;
		MarkPending(kPendingPump);
	}

	void BridgeApi::SetBridgeAvailability(bool a_available)
	{
		std::lock_guard lock(m_mutex);
		assert(!a_available || m_bridge);
		if (m_bridgeAvailable.exchange(a_available, std::memory_order_acq_rel) != a_available) {
			m_readyFired = false;
		}
		if (!a_available) {
			m_queuedReplies.clear();  // nothing left to settle them against
		}
		MarkPending(kPendingPump);
	}

	void BridgeApi::PumpRuntimeCallbacks()
	{
		const auto reasons = m_pending.fetch_and(~kPendingPump, std::memory_order_acq_rel);
		if (!(reasons & kPendingPump)) return;
		MessageBridge* bridge = nullptr;
		std::vector<std::pair<std::string, Registration>> sendsToRegister;
		std::vector<std::pair<std::string, RequestRegistration>> requestsToRegister;
		std::vector<PendingSend> sends;
		std::vector<QueuedReply> replies;
		{
			std::lock_guard lock(m_mutex);
			bridge = m_bridge;
			if (bridge && m_dirty) {
				for (const auto& item : m_sends) {
					sendsToRegister.push_back(item);
				}
				for (const auto& item : m_requests) {
					requestsToRegister.push_back(item);
				}
				m_dirty = false;
			}
			if (m_bridgeAvailable.load(std::memory_order_acquire)) {
				// Single stable compaction pass: retained sends keep their order.
				auto kept = m_pendingSends.begin();
				for (auto& send : m_pendingSends) {
					if (const auto* canonical = FindIdCaseInsensitive(m_instantiatedViews, send.view)) {
						send.view = *canonical;
						sends.push_back(std::move(send));
					} else if (!m_viewCatalogReady || FindIdCaseInsensitive(m_knownViews, send.view)) {
						if (&*kept != &send) *kept = std::move(send);
						++kept;
					}
				}
				m_pendingSends.erase(kept, m_pendingSends.end());
				replies.swap(m_queuedReplies);
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
				if (reply.rejected) {
					bridge->RejectTo(reply.token, reply.code, reply.message);
				} else {
					bridge->RespondJsonTo(reply.token, reply.payloadJson);
				}
			}
		}
		// Decide under the dispatch lock so a replaced or reset callback is never invoked stale.
		std::lock_guard dispatchLock(m_callbackDispatchMutex);
		ReadyFn ready = nullptr;
		void* readyUser = nullptr;
		{
			std::lock_guard lock(m_mutex);
			if (m_bridgeAvailable.load(std::memory_order_acquire) && !m_readyFired) {
				m_readyFired = true;
				ready = m_readyCb;
				readyUser = m_readyUser;
			}
		}
		if (ready) ready(readyUser);
	}
}
