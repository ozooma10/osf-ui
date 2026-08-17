#include "API/BridgeApi.h"

#include "Core/Version.h"
#include "Core/Ids.h"            // qualified view id shape — the synchronous RegisterView gate
#include "Bridge/MessageBridge.h"  // also pulls nlohmann/json
#include "Core/Json.h"           // Dump — never a bare dump() on a tick path
#include "Settings/SettingsStore.h"  // ValidateSchemaShape — the synchronous shape gate

namespace OSFUI::API
{
	namespace
	{
		// Endpoint shape (api-freeze item 3): a plugin endpoint is
		// "<modId>.<name>" with modId the item-1 "<author>.<modname>" grammar, so
		// every registrable name carries two dots minimum. That makes platform
		// endpoints structurally unregisterable (dotless verbs like "close",
		// single-dot "menu.open"/"osfui.gamepadRaw") without a
		// reserved-prefix list that could drift. The mod id must be pattern-valid
		// but need not have a registered schema; the name after it is free-form
		// and may contain dots ("acme.mymod.catalog.get").
		bool IsValidPluginEndpointName(std::string_view a_name)
		{
			const auto first = a_name.find('.');
			if (first == std::string_view::npos) {
				return false;
			}
			const auto second = a_name.find('.', first + 1);
			if (second == std::string_view::npos || second + 1 >= a_name.size()) {
				return false;
			}
			return Ids::IsValidModId(a_name.substr(0, second));
		}

		// Cap on queued SendToWeb messages per target view while no target page is
		// instantiated to receive them (ABI 1.3 queue-until-deliverable). Matches the
		// renderer's per-view queue bound; overflow drops the oldest so the view
		// still converges on the newest pushed state when it comes up.
		constexpr std::size_t kMaxPendingSendsPerView = 64;

		// Cap on queued health reports awaiting the main tick (ABI 1.7). The
		// registry has its own caps, so this only bounds the window between a
		// producer's call and the drain — normally one frame. A producer looping
		// on ReportIssue off-thread hits this instead of growing memory; the
		// overflow is refused (false) rather than evicting, because dropping an
		// older op could drop a report while keeping the clear that cancels it.
		constexpr std::size_t kMaxPendingHealthIssueOps = 256;
		constexpr std::size_t kMaxInflightRequestsPerView = 64;
		constexpr auto kRequestTimeout = std::chrono::seconds(30);

		// Shared front half of every ABI 1.7 health-reporting call: the source of an
		// issue is the CALLER's mod id, never a payload field, so a mod cannot
		// file a report against someone else or against a platform source.
		bool ValidateHealthReporter(std::string_view a_fn, const char* a_modId)
		{
			if (!a_modId || !a_modId[0]) {
				REX::WARN("BridgeApi: [content] refused {} — no mod id", a_fn);
				return false;
			}
			if (!Ids::IsValidModId(a_modId)) {
				REX::WARN("BridgeApi: [content] refused {}('{}') — mod ids are '<author>.<modname>' "
						  "(lowercase [a-z0-9-] segments)",
					a_fn, std::string_view(a_modId).substr(0, 128));
				return false;
			}
			return true;
		}
	}

	BridgeApi& BridgeApi::Get()
	{
		// Process-lifetime API state can be in use by plugin threads when Windows
		// begins DLL detach. Do not destruct its mutexes, callbacks, or mirrors
		// after those threads have been stopped.
		static BridgeApi* const instance = new BridgeApi;
		return *instance;
	}

	std::uint32_t BridgeApi::GetInterfaceVersion()
	{
		return kBridgeAPIVersion;
	}

	void BridgeApi::GetPluginVersion(std::uint32_t& a_major, std::uint32_t& a_minor, std::uint32_t& a_patch)
	{
		a_major = kOsfuiReleaseVersionMajor;
		a_minor = kOsfuiReleaseVersionMinor;
		a_patch = kOsfuiReleaseVersionPatch;
	}

	const char* BridgeApi::GetBridgeProtocolVersion()
	{
		return kBridgeProtocolVersion;  // static string literal; valid for process lifetime
	}

	bool BridgeApi::IsBridgeReady()
	{
		return _bridgeAvailable.load();
	}

	void BridgeApi::RegisterSend(const char* a_name, SendFn a_handler, void* a_user)
	{
		if (!a_name || !a_handler) {
			return;
		}
		const std::string name(a_name);
		if (!IsValidPluginEndpointName(name)) {
			REX::WARN("BridgeApi: [content] refused RegisterSend('{}') — sends are '<author>.<modname>.<name>' "
					  "(two dots minimum; the leading mod id follows the item-1 grammar). "
					  "Single-dot and dotless names are the platform's",
				name.substr(0, 128));
			return;
		}
		std::lock_guard lock(_mutex);
		// First-wins: a duplicate registration is refused, not last-writer-wins,
		// so an already-claimed endpoint cannot be hijacked.
		// Replacing your own handler means UnregisterSend then re-register;
		// the pair works back-to-back within one tick.
		if (_sends.contains(name) || _requests.contains(name) || _legacyCommands.contains(name) ||
			_legacyRequests.contains(name)) {
			REX::WARN("BridgeApi: [content] refused RegisterSend('{}') — already registered (first wins; "
					  "UnregisterSend first to replace your own handler)",
				name);
			return;
		}
		_sends[name] = { a_handler, a_user };
		std::erase(_pendingSendUnregister, name);  // cancel a pending removal of the same id
		_dirty = true;
	}

	void BridgeApi::UnregisterSend(const char* a_name)
	{
		if (!a_name) {
			return;
		}
		const std::string name(a_name);
		std::lock_guard lock(_mutex);
		if (_sends.erase(name) > 0) {
			_pendingSendUnregister.push_back(name);
			_dirty = true;
		}
	}

	void BridgeApi::RegisterRequest(const char* a_name, RequestFn a_handler, void* a_user)
	{
		if (!a_name || !a_handler) return;
		const std::string name(a_name);
		if (!IsValidPluginEndpointName(name)) {
			REX::WARN("BridgeApi: [content] refused RegisterRequest('{}') — requests are '<author>.<modname>.<name>'", name.substr(0, 128));
			return;
		}
		std::lock_guard lock(_mutex);
		if (_sends.contains(name) || _requests.contains(name) || _legacyCommands.contains(name) ||
			_legacyRequests.contains(name)) {
			REX::WARN("BridgeApi: [content] refused RegisterRequest('{}') — already registered (first wins across sends and requests)", name);
			return;
		}
		_requests[name] = { a_handler, a_user };
		std::erase(_pendingRequestUnregister, name);
		_dirty = true;
	}

	void BridgeApi::RegisterLegacyCommand(const char* a_name, SendFn a_handler, void* a_user)
	{
		if (!a_name || !a_handler) return;
		const std::string name(a_name);
		if (!IsValidPluginEndpointName(name)) {
			REX::WARN("BridgeApi: [content] refused legacy RegisterCommand('{}') — commands are '<author>.<modname>.<name>'",
				name.substr(0, 128));
			return;
		}
		std::lock_guard lock(_mutex);
		if (_sends.contains(name) || _requests.contains(name) || _legacyCommands.contains(name) ||
			_legacyRequests.contains(name)) {
			REX::WARN("BridgeApi: [content] refused legacy RegisterCommand('{}') — already registered (first wins)", name);
			return;
		}
		_legacyCommands[name] = { a_handler, a_user };
		std::erase(_pendingLegacyCommandUnregister, name);
		_dirty = true;
	}

	bool BridgeApi::RegisterLegacyRequest(const char* a_name, RequestFn a_handler, void* a_user)
	{
		if (!a_name || !a_handler) return false;
		const std::string name(a_name);
		if (!IsValidPluginEndpointName(name)) return false;
		std::lock_guard lock(_mutex);
		if (_sends.contains(name) || _requests.contains(name) || _legacyCommands.contains(name) ||
			_legacyRequests.contains(name)) {
			return false;
		}
		_legacyRequests[name] = { a_handler, a_user, true };
		std::erase(_pendingLegacyRequestUnregister, name);
		_dirty = true;
		return true;
	}

	void BridgeApi::UnregisterLegacyRequest(const char* a_name)
	{
		if (!a_name) return;
		const std::string name(a_name);
		std::lock_guard lock(_mutex);
		if (_legacyRequests.erase(name) > 0) {
			_pendingLegacyRequestUnregister.push_back(name);
			_dirty = true;
		}
	}

	void BridgeApi::UnregisterLegacyCommand(const char* a_name)
	{
		if (!a_name) return;
		const std::string name(a_name);
		std::lock_guard lock(_mutex);
		if (_legacyCommands.erase(name) > 0) {
			_pendingLegacyCommandUnregister.push_back(name);
			_dirty = true;
		}
	}

	void BridgeApi::UnregisterRequest(const char* a_name)
	{
		if (!a_name) return;
		const std::string name(a_name);
		std::lock_guard lock(_mutex);
		if (_requests.erase(name) > 0) {
			_pendingRequestUnregister.push_back(name);
			_dirty = true;
		}
	}
	bool BridgeApi::SendToWeb(const char* a_viewId, const char* a_type, const char* a_payloadJson)
	{
		if (!a_viewId || !a_type || !a_payloadJson) {
			return false;
		}
		// Validate now so a malformed payload is reported synchronously; delivery is
		// marshaled to the main thread in PumpMainThread.
		if (!Json::Parse(a_payloadJson)) {
			return false;
		}
		std::lock_guard lock(_mutex);
		// ABI 1.3 queues until the target page exists, not merely until any bridge
		// is available. Discovered-but-uninstantiated views can therefore retain
		// bounded first-open state while unrelated views are already running.
		std::size_t sameView = 0;
		for (const auto& s : _pendingSends) {
			sameView += (s.view == a_viewId) ? 1u : 0u;
		}
		if (sameView >= kMaxPendingSendsPerView) {
			const auto oldest = std::ranges::find_if(_pendingSends,
				[&](const PendingSend& s) { return s.view == a_viewId; });
			REX::WARN("BridgeApi: SendToWeb holdback for view '{}' is full ({}); dropping oldest queued '{}'",
				a_viewId, kMaxPendingSendsPerView, oldest->type);
			_pendingSends.erase(oldest);
		}
		_pendingSends.push_back({ std::string(a_viewId), std::string(a_type), std::string(a_payloadJson) });
		return true;
	}

	bool BridgeApi::SetViewState(const char* a_modId, const char* a_key, const char* a_payloadJson)
	{
		if (!a_modId || !a_key || !a_payloadJson || !a_key[0]) {
			return false;
		}
		if (!Ids::IsAcceptedModId(a_modId)) {
			REX::WARN("BridgeApi: [content] refused SetViewState('{}') — mod ids are '<author>.<modname>'",
				std::string_view(a_modId).substr(0, 128));
			return false;
		}
		// Bound the key like every other content-supplied name: it is echoed on
		// the wire and used as a cache key.
		const std::string key(a_key);
		if (key.size() > 128) {
			REX::WARN("BridgeApi: [content] refused SetViewState — key longer than 128 characters");
			return false;
		}
		// Validate and parse OUTSIDE the mutex (this is callable from any
		// thread), then take it only to queue. Any JSON VALUE is legal, not just
		// an object: a state key may perfectly well be a number or an array.
		auto parsed = Json::Parse(a_payloadJson);
		if (!parsed) {
			REX::WARN("BridgeApi: [content] refused SetViewState('{}.{}') — payload is not valid JSON", a_modId, key);
			return false;
		}
		std::lock_guard lock(_mutex);
		constexpr std::size_t kMaxPendingStateOps = 256;
		if (_pendingStateOps.size() >= kMaxPendingStateOps) {
			REX::WARN("BridgeApi: pending SetViewState queue full; dropping '{}.{}'", a_modId, key);
			return false;
		}
		_pendingStateOps.push_back(ViewStateOp{ std::string(a_modId), key, std::move(*parsed) });
		return true;
	}

	void BridgeApi::NoteLegacyApiCaller(std::string a_moduleName, std::uint32_t a_major,
		std::uint32_t a_minor, bool a_supported)
	{
		std::lock_guard lock(_mutex);
		// Bounded, and deduped by module: a plugin that retries on every load
		// screen must not grow this, and the player needs one issue per mod.
		// This set is emptied by every drain, so RuntimeHealthCoordinator re-applies
		// the same two rules on the side that keeps them for the session.
		for (const auto& seen : _legacyCallers) {
			if (seen.module == a_moduleName && seen.supported == a_supported) {
				return;
			}
		}
		if (_legacyCallers.size() >= kMaxLegacyCallers) {
			return;
		}
		_legacyCallers.push_back(LegacyCaller{ std::move(a_moduleName), a_major, a_minor, a_supported });
	}

	std::vector<BridgeApi::LegacyCaller> BridgeApi::TakeLegacyApiCallers()
	{
		std::lock_guard lock(_mutex);
		std::vector<LegacyCaller> out;
		out.swap(_legacyCallers);
		return out;
	}

	std::vector<BridgeApi::ViewStateOp> BridgeApi::TakeViewStateOps()
	{
		std::lock_guard lock(_mutex);
		std::vector<ViewStateOp> out;
		out.swap(_pendingStateOps);
		return out;
	}

	void BridgeApi::SetReadyCallback(ReadyFn a_callback, void* a_user)
	{
		std::unique_lock lock(_mutex);
		if (_readyInvoking && _readyInvokingThread != std::this_thread::get_id()) {
			_readyInvokeCv.wait(lock, [this] { return !_readyInvoking; });
		}
		_readyCb = a_callback;
		_readyUser = a_user;
		// If the bridge is already available, re-arm so Pump fires the new callback on
		// the next (main-thread) tick rather than dropping it.
		if (_bridgeAvailable.load()) {
			_readyFired = false;
		}
	}

	bool BridgeApi::RequestMenu(const char* a_viewId, bool a_open)
	{
		if (!a_viewId || !a_viewId[0]) {
			return false;
		}
		const std::string id(a_viewId);
		std::lock_guard lock(_mutex);
		// Truthful queue-time contract: opens accept anything discovered at boot
		// (Runtime will instantiate it on demand); closes accept only an instantiated view and
		// never cause an uninstantiated view to be created.
		if (a_open ? !_knownViews.contains(id) : !_instantiatedViews.contains(id)) {
			return false;
		}
		_pendingViewPresentationRequests.push_back({ id, a_open });
		return true;
	}

	void BridgeApi::SetViewCatalog(const std::vector<std::string>& a_viewIds)
	{
		std::lock_guard lock(_mutex);
		_knownViews.clear();
		_knownViews.insert(a_viewIds.begin(), a_viewIds.end());
		_instantiatedViews.clear();
		_viewCatalogReady = true;
	}

	void BridgeApi::SetViewInstantiated(std::string_view a_viewId, bool a_instantiated)
	{
		std::lock_guard lock(_mutex);
		if (a_instantiated) {
			_instantiatedViews.emplace(a_viewId);
		} else {
			_instantiatedViews.erase(std::string(a_viewId));
			std::erase_if(_inflightRequests, [&](const auto& entry) { return entry.second.view == a_viewId; });
		}
	}

	std::uint32_t BridgeApi::SubscribeSettings(const char* a_modId, SettingChangedFn a_fn, void* a_user)
	{
		return _subscriptions.Subscribe(a_modId, a_fn, a_user);
	}

	void BridgeApi::UnsubscribeSettings(std::uint32_t a_token)
	{
		_subscriptions.Unsubscribe(a_token);
	}

	bool BridgeApi::GetSettingBool(const char* a_modId, const char* a_key, bool* a_out)
	{
		return _mirror.GetBool(a_modId, a_key, a_out);
	}

	bool BridgeApi::GetSettingInt(const char* a_modId, const char* a_key, std::int64_t* a_out)
	{
		return _mirror.GetInt(a_modId, a_key, a_out);
	}

	bool BridgeApi::GetSettingFloat(const char* a_modId, const char* a_key, double* a_out)
	{
		return _mirror.GetFloat(a_modId, a_key, a_out);
	}

	std::uint32_t BridgeApi::GetSettingString(const char* a_modId, const char* a_key, char* a_buf, std::uint32_t a_bufLen)
	{
		return _mirror.GetString(a_modId, a_key, a_buf, a_bufLen);
	}

	std::uint32_t BridgeApi::SubscribeHotkey(const char* a_modId, const char* a_key, HotkeyFn a_fn, void* a_user)
	{
		return _hotkeys.Subscribe(a_modId, a_key, a_fn, a_user);
	}

	void BridgeApi::UnsubscribeHotkey(std::uint32_t a_token)
	{
		_hotkeys.Unsubscribe(a_token);
	}

	bool BridgeApi::RegisterSettingsSchema(const char* a_schemaJson)
	{
		if (!a_schemaJson) {
			return false;
		}
		// Parse and shape errors report synchronously (ABI contract); the store
		// merge is marshaled to the main tick (Runtime::DrainSchemaOps), where
		// precedence resolves with a log warning.
		auto parsedSchema = Json::Parse(a_schemaJson);
		if (!parsedSchema) {
			REX::WARN("BridgeApi: [content] RegisterSettingsSchema rejected — malformed JSON");
			return false;
		}
		auto& schema = *parsedSchema;
		if (!SettingsStore::ValidateSchemaShape(schema)) {
			return false;  // warned inside
		}
		std::lock_guard lock(_mutex);
		_pendingSchemaOps.push_back({ std::move(schema), {} });
		return true;
	}

	void BridgeApi::UnregisterSettingsSchema(const char* a_modId)
	{
		if (!a_modId || !a_modId[0]) {
			return;
		}
		std::lock_guard lock(_mutex);
		_pendingSchemaOps.push_back({ nlohmann::json{}, std::string(a_modId) });
	}

	std::vector<BridgeApi::SchemaOp> BridgeApi::TakeSchemaOps()
	{
		std::lock_guard lock(_mutex);
		std::vector<SchemaOp> out;
		out.swap(_pendingSchemaOps);
		return out;
	}

	std::vector<BridgeApi::ViewPresentationRequest> BridgeApi::TakeViewPresentationRequests()
	{
		std::lock_guard lock(_mutex);
		std::vector<ViewPresentationRequest> out;
		out.swap(_pendingViewPresentationRequests);
		return out;
	}

	bool BridgeApi::RegisterView(const char* a_viewId)
	{
		if (!a_viewId || !a_viewId[0]) {
			return false;
		}
		// Synchronous shape gate: view ids are qualified
		// "<author>.<modname>/<view>" (api-freeze item 1). A structurally invalid
		// id can never match a discovered manifest, so refuse it here where the
		// caller sees the false.
		if (!Ids::IsValidQualifiedViewId(a_viewId)) {
			REX::WARN("BridgeApi: [content] refused RegisterView('{}') — view ids are qualified "
					  "'<author>.<modname>/<view>' (lowercase [a-z0-9-] segments)",
				std::string_view(a_viewId).substr(0, 128));
			return false;
		}
		// Runtime drains this on the main tick (DrainViewRegistrations), where the
		// manifest lookup happens and openOnStart is applied; a not-found id warns
		// there, not here.
		std::lock_guard lock(_mutex);
		_pendingViewRegs.emplace_back(a_viewId);
		return true;
	}

	std::vector<std::string> BridgeApi::TakeViewRegistrations()
	{
		std::lock_guard lock(_mutex);
		std::vector<std::string> out;
		out.swap(_pendingViewRegs);
		return out;
	}

	bool BridgeApi::ReportIssue(const char* a_modId, const char* a_id, const char* a_code,
		std::uint32_t a_severity, const char* a_subject, const char* a_contextJson)
	{
		if (!ValidateHealthReporter("ReportIssue", a_modId)) {
			return false;
		}
		if (!a_id || !a_id[0] || !a_code || !a_code[0]) {
			REX::WARN("BridgeApi: [content] refused ReportIssue from '{}' — both an id (the dedupe key) "
					  "and a code (the kind of condition) are required",
				a_modId);
			return false;
		}
		// Context is optional, but a non-object is a producer bug worth reporting
		// synchronously: silently dropping it would leave an issue with no detail
		// and no explanation of why.
		nlohmann::json context = nlohmann::json::object();
		if (a_contextJson && a_contextJson[0]) {
			auto parsed = Json::Parse(a_contextJson);
			if (!parsed || !parsed->is_object()) {
				REX::WARN("BridgeApi: [content] refused ReportIssue('{}', '{}') — context must be a JSON object",
					a_modId, a_id);
				return false;
			}
			context = std::move(*parsed);
		}
		// Severity is a closed two-value set; a value from a header newer than this
		// OSF UI runtime is treated as the worst tier we know, so a future "critical" cannot
		// arrive looking milder than it is.
		if (a_severity > 1u) {
			REX::WARN("BridgeApi: [content] ReportIssue('{}', '{}') — unknown severity {}, treating as error",
				a_modId, a_id, a_severity);
		}

		std::lock_guard lock(_mutex);
		if (_pendingHealthIssueOps.size() >= kMaxPendingHealthIssueOps) {
			REX::WARN("BridgeApi: [content] refused ReportIssue('{}', '{}') — {} health reports already queued "
					  "for this tick; is a producer reporting in a loop?",
				a_modId, a_id, kMaxPendingHealthIssueOps);
			return false;
		}
		_pendingHealthIssueOps.push_back(HealthIssueOp{
			.kind = HealthIssueOp::Kind::kReport,
			.modId = a_modId,
			.id = a_id,
			.code = a_code,
			.error = a_severity >= 1u,
			.subject = a_subject ? a_subject : "",
			.context = std::move(context),
		});
		return true;
	}

	bool BridgeApi::ClearIssue(const char* a_modId, const char* a_id)
	{
		if (!ValidateHealthReporter("ClearIssue", a_modId)) {
			return false;
		}
		if (!a_id || !a_id[0]) {
			return false;
		}
		std::lock_guard lock(_mutex);
		if (_pendingHealthIssueOps.size() >= kMaxPendingHealthIssueOps) {
			return false;  // warned by the report path; a clear storm implies one
		}
		_pendingHealthIssueOps.push_back(HealthIssueOp{
			.kind = HealthIssueOp::Kind::kClear,
			.modId = a_modId,
			.id = a_id,
		});
		return true;
	}

	bool BridgeApi::ClearIssuesExcept(const char* a_modId, const char* a_keepIdsJson)
	{
		if (!ValidateHealthReporter("ClearIssuesExcept", a_modId)) {
			return false;
		}
		std::vector<std::string> keep;
		if (a_keepIdsJson && a_keepIdsJson[0]) {
			const auto parsed = Json::Parse(a_keepIdsJson);
			if (!parsed || !parsed->is_array()) {
				REX::WARN("BridgeApi: [content] refused ClearIssuesExcept('{}') — keep list must be a JSON array of ids",
					a_modId);
				return false;
			}
			for (const auto& entry : *parsed) {
				if (!entry.is_string()) {
					REX::WARN("BridgeApi: [content] refused ClearIssuesExcept('{}') — keep list holds a non-string entry",
						a_modId);
					return false;
				}
				keep.push_back(entry.get<std::string>());
			}
		}
		std::lock_guard lock(_mutex);
		if (_pendingHealthIssueOps.size() >= kMaxPendingHealthIssueOps) {
			return false;
		}
		_pendingHealthIssueOps.push_back(HealthIssueOp{
			.kind = HealthIssueOp::Kind::kClearExcept,
			.modId = a_modId,
			.keep = std::move(keep),
		});
		return true;
	}

	std::vector<BridgeApi::HealthIssueOp> BridgeApi::TakeHealthIssueOps()
	{
		std::lock_guard lock(_mutex);
		std::vector<HealthIssueOp> out;
		out.swap(_pendingHealthIssueOps);
		return out;
	}

	void BridgeApi::RespondThunk(std::uint64_t token, const char* type, const char* json) noexcept { Get().RespondRequest(token, type, json); }
	void BridgeApi::RejectThunk(std::uint64_t token, const char* code, const char* message) noexcept { Get().RejectRequest(token, code, message); }

	void BridgeApi::RespondRequest(std::uint64_t token, const char* type, const char* json) noexcept
	{
		const auto parsed = json ? Json::Parse(json) : std::nullopt;
		std::lock_guard lock(_mutex);
		const auto it = _inflightRequests.find(token);
		if (it == _inflightRequests.end()) { REX::WARN("BridgeApi: ignored late response for stale token {}", token); return; }
		if (it->second.answered) { REX::WARN("BridgeApi: ignored second response for request '{}'", it->second.name); return; }
		it->second.answered = true;
		if (!parsed) {
			it->second.rejected = true; it->second.code = "invalid-response"; it->second.message = "plugin returned invalid JSON"; return;
		}
		it->second.type = (type && type[0]) ? type : it->second.name;
		it->second.payloadJson = Json::Dump(*parsed);
	}

	void BridgeApi::RejectRequest(std::uint64_t token, const char* code, const char* message) noexcept
	{
		std::lock_guard lock(_mutex);
		const auto it = _inflightRequests.find(token);
		if (it == _inflightRequests.end()) { REX::WARN("BridgeApi: ignored late rejection for stale token {}", token); return; }
		if (it->second.answered) { REX::WARN("BridgeApi: ignored second response for request '{}'", it->second.name); return; }
		it->second.answered = true; it->second.rejected = true;
		it->second.code = (code && code[0]) ? code : "plugin-error";
		it->second.message = message ? message : "";
	}

	void BridgeApi::DispatchRequest(const std::string& name, const RequestRegistration& reg,
		const nlohmann::json& payload, MessageBridge& bridge)
	{
		const std::string view(bridge.CurrentSource());
		// A request always carries an id in 2.0 — the bridge rejects one that
		// does not before dispatch ever happens — so there is no
		// "request-id-required" case left to answer.
		Request request;
		std::string payloadJson;
		{
			std::lock_guard lock(_mutex);
			const auto count = static_cast<std::size_t>(std::ranges::count_if(
				_inflightRequests, [&](const auto& e) { return e.second.view == view; }));
			if (count >= kMaxInflightRequestsPerView) {
				bridge.Reject("request-capacity", "too many requests are already in flight"); return;
			}
		}
		// Take ownership of the correlation id before the plugin can answer:
		// it replies through Request::Respond/Reject, whenever it gets there,
		// and PumpMainThread settles it (or expires it at the OSF UI runtime deadline).
		// The bridge's token — not the page's request id, which collides
		// across documents — is what settles it.
		const std::string deferToken = bridge.Defer();
		{
			std::lock_guard lock(_mutex);
			// The payload is the caller's own object, verbatim: routing metadata
			// lives beside it on the envelope now, so there is no `command` field
			// to strip out of it first.
			payloadJson = Json::Dump(payload);
			const auto token = _nextRequestToken++;
			InflightRequest inflight;
			inflight.token = token;
			inflight.view = view;
			inflight.deferToken = deferToken;
			inflight.name = name;
			inflight.legacyReply = reg.legacy && bridge.IsLegacyApiView(view);
			inflight.deadline = std::chrono::steady_clock::now() + kRequestTimeout;
			_inflightRequests.emplace(token, std::move(inflight));
			request.command = name.c_str(); request.payloadJson = payloadJson.c_str(); request.sourceViewId = view.c_str();
			request._token = token; request._respond = &RespondThunk; request._reject = &RejectThunk;
		}
		reg.fn(request, reg.user);
	}
	void BridgeApi::SetBridgeAvailability(MessageBridge* a_bridge)
	{
		std::lock_guard lock(_mutex);
		_bridge = a_bridge;  // a change (incl. null<->ptr) is detected in Pump and forces a re-apply
		if (!a_bridge) _inflightRequests.clear();
	}

	void BridgeApi::PumpMainThread(std::chrono::steady_clock::time_point now)
	{
		MessageBridge* bridge = nullptr;
		std::vector<std::string> sendRemovals, requestRemovals,
			legacyCommandRemovals, legacyRequestRemovals;
		std::vector<std::pair<std::string, Registration>> sendRegistrations;
		std::vector<std::pair<std::string, Registration>> legacyCommands;
		std::vector<std::pair<std::string, RequestRegistration>> requests;
		std::vector<std::pair<std::string, RequestRegistration>> legacyRequests;
		std::vector<PendingSend> sends;
		std::vector<PendingReply> replies;
		bool fireReady = false; ReadyFn readyCb = nullptr; void* readyUser = nullptr;
		{
			std::lock_guard lock(_mutex); bridge = _bridge;
			if (bridge) {
				const bool changed = bridge != _appliedBridge;
				if (changed || _dirty) {
					if (changed) {
						_pendingSendUnregister.clear();
						_pendingRequestUnregister.clear();
						_pendingLegacyCommandUnregister.clear();
						_pendingLegacyRequestUnregister.clear();
					} else {
						sendRemovals.swap(_pendingSendUnregister);
						requestRemovals.swap(_pendingRequestUnregister);
						legacyCommandRemovals.swap(_pendingLegacyCommandUnregister);
						legacyRequestRemovals.swap(_pendingLegacyRequestUnregister);
					}
					for (const auto& pair : _sends) sendRegistrations.push_back(pair);
					for (const auto& pair : _legacyCommands) legacyCommands.push_back(pair);
					for (const auto& pair : _requests) requests.push_back(pair);
					for (const auto& pair : _legacyRequests) legacyRequests.push_back(pair);
					_appliedBridge = bridge; _dirty = false;
				}
				// Retain known discovered-but-uninstantiated targets until Runtime marks their view instance
				// instantiated. Unknown ids keep the historical drop behavior once discovery
				// has supplied an authoritative catalog.
				for (auto it = _pendingSends.begin(); it != _pendingSends.end();) {
					if (_instantiatedViews.contains(it->view)) {
						sends.push_back(std::move(*it));
						it = _pendingSends.erase(it);
					} else if (_viewCatalogReady && !_knownViews.contains(it->view)) {
						REX::DEBUG("BridgeApi: SendToWeb target '{}' was not discovered; dropped", it->view);
						it = _pendingSends.erase(it);
					} else {
						++it;
					}
				}
				if (!_readyFired) {
					_readyFired = true;
					fireReady = true;
					readyCb = _readyCb;
					readyUser = _readyUser;
				}
				for (auto it = _inflightRequests.begin(); it != _inflightRequests.end();) {
					auto& req = it->second;
					if (!req.answered && now < req.deadline) { ++it; continue; }
					PendingReply reply;
					reply.view = req.view;
					reply.deferToken = req.deferToken;
					reply.name = req.name;
					reply.type = req.type;
					reply.legacyReply = req.legacyReply;
					if (!req.answered) { reply.rejected = true; reply.code = "no-response"; reply.message = "the plugin never answered"; }
					else if (req.rejected) { reply.rejected = true; reply.code = req.code; reply.message = req.message; }
					else { reply.payloadJson = req.payloadJson; }
					replies.push_back(std::move(reply)); it = _inflightRequests.erase(it);
				}
			}
		}
		if (bridge) {
			for (const auto& name : sendRemovals) bridge->UnregisterSend(name);
			for (const auto& name : requestRemovals) bridge->UnregisterRequest(name);
			for (const auto& name : legacyCommandRemovals) bridge->UnregisterLegacyCommand(name);
			for (const auto& name : legacyRequestRemovals) bridge->UnregisterRequest(name);
			for (const auto& [name, reg] : sendRegistrations) bridge->RegisterSend(name, [name, reg](const nlohmann::json& payload, MessageBridge& b) {
				const auto dump = Json::Dump(payload);
				const std::string src(b.CurrentSource()); reg.fn(name.c_str(), dump.c_str(), src.c_str(), reg.user);
			});
			for (const auto& [name, reg] : legacyCommands) {
				bridge->RegisterLegacyCommand(name, [name, reg](const nlohmann::json& payload, MessageBridge& b) {
					const auto dump = Json::Dump(payload);
					const std::string src(b.CurrentSource());
					reg.fn(name.c_str(), dump.c_str(), src.c_str(), reg.user);
				});
			}
			for (const auto& [name, reg] : requests) bridge->RegisterRequest(name, [this, name, reg](const nlohmann::json& payload, MessageBridge& b) { DispatchRequest(name, reg, payload, b); });
			for (const auto& [name, reg] : legacyRequests) bridge->RegisterRequest(name,
				[this, name, reg](const nlohmann::json& payload, MessageBridge& b) {
					DispatchRequest(name, reg, payload, b);
				});
			// A plugin push is an EVENT: unsolicited, one-shot, never replayed.
			// Mod-backend-owned data that changes over time belongs in SetViewState.
			for (const auto& send : sends) bridge->EmitJson(send.view, send.type, send.payloadJson);
			for (const auto& reply : replies) {
				if (reply.rejected) {
					REX::WARN("BridgeApi: request '{}' from view '{}' -> {}", reply.name, reply.view, reply.code);
					// No ReportProtocolFault(): a plugin answering `Reject` is the endpoint
					// working as designed, and the code it chose is its own. The
					// page already gets the typed error from RejectTo, and the
					// helper prints it to the view's console — routing it through
					// the misuse sink as well made ordinary application errors
					// accumulate toward a `view.protocol-misuse` issue blaming a
					// view that did nothing wrong.
					bridge->RejectTo(reply.deferToken, reply.code, reply.message);
				} else {
					if (reply.legacyReply) {
						auto payload = Json::Parse(reply.payloadJson).value_or(nlohmann::json::object());
						bridge->RespondTo(reply.deferToken, {
							{ "__osfuiV1Reply", {
								{ "type", reply.type.empty() ? reply.name : reply.type },
								{ "payload", std::move(payload) },
							} },
						});
					} else {
						bridge->RespondJsonTo(reply.deferToken, reply.payloadJson);
					}
				}
			}
		}
		_bridgeAvailable.store(bridge != nullptr);
		bool invokeReady = false;
		if (fireReady && readyCb) {
			// Close the snapshot-to-call race under the same mutex used by
			// SetReadyCallback. A replacement is deferred to the next pump;
			// once armed here, an off-thread replacement waits for completion.
			std::lock_guard lock(_mutex);
			if (_readyCb == readyCb && _readyUser == readyUser) {
				_readyInvoking = true;
				_readyInvokingThread = std::this_thread::get_id();
				invokeReady = true;
			}
		}
		if (invokeReady) {
			readyCb(readyUser);
			{
				std::lock_guard lock(_mutex);
				_readyInvoking = false;
				_readyInvokingThread = {};
			}
			_readyInvokeCv.notify_all();
		}
		_subscriptions.Pump(_mirror); _hotkeys.Pump();
	}
}
