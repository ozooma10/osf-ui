#include "api/BridgeApi.h"

#include "core/Version.h"
#include "runtime/Ids.h"            // qualified view id shape — the synchronous RegisterView gate
#include "runtime/MessageBridge.h"  // also pulls nlohmann/json
#include "runtime/SettingsStore.h"  // ValidateSchemaShape — the synchronous shape gate

namespace OSFUI::API
{
	namespace
	{
		// Command shape (api-freeze item 3, ABI 1.6): a plugin command is
		// "<modId>.<name>" with modId the item-1 "<author>.<modname>" grammar, so
		// every registrable command carries two dots minimum. That makes platform
		// commands structurally unregisterable (dotless verbs like "close",
		// single-dot "menu.open"/"game.get"/"osfui.gamepadRaw") without a
		// reserved-prefix list that could drift. The mod id must be pattern-valid
		// but need not have a registered schema; the name after it is free-form
		// and may contain dots ("acme.mymod.catalog.get").
		bool IsValidPluginCommand(std::string_view a_cmd)
		{
			const auto first = a_cmd.find('.');
			if (first == std::string_view::npos) {
				return false;
			}
			const auto second = a_cmd.find('.', first + 1);
			if (second == std::string_view::npos || second + 1 >= a_cmd.size()) {
				return false;
			}
			return Ids::IsValidModId(a_cmd.substr(0, second));
		}

		// Cap on queued SendToWeb messages per target view while no bridge is live
		// to flush them (ABI 1.3 queue-until-deliverable). Matches the renderer's
		// per-view queue bound; overflow drops the oldest so the view still
		// converges on the newest pushed state when it comes up.
		constexpr std::size_t kMaxPendingSendsPerView = 64;

		// Cap on queued health reports awaiting the main tick (ABI 1.7). The
		// registry has its own caps, so this only bounds the window between a
		// producer's call and the drain — normally one frame. A producer looping
		// on ReportIssue off-thread hits this instead of growing memory; the
		// overflow is refused (false) rather than evicting, because dropping an
		// older op could drop a report while keeping the clear that cancels it.
		constexpr std::size_t kMaxPendingDiagnosticOps = 256;

		// Shared front half of every ABI 1.7 diagnostics call: the source of an
		// issue is the CALLER's mod id, never a payload field, so a mod cannot
		// file a report against someone else or against a platform source.
		bool ValidateDiagnosticCaller(std::string_view a_fn, const char* a_modId)
		{
			if (!a_modId || !a_modId[0]) {
				REX::WARN("BridgeApi: refused {} — no mod id", a_fn);
				return false;
			}
			if (!Ids::IsValidModId(a_modId)) {
				REX::WARN("BridgeApi: refused {}('{}') — mod ids are '<author>.<modname>' "
						  "(lowercase [a-z0-9-] segments)",
					a_fn, std::string_view(a_modId).substr(0, 128));
				return false;
			}
			return true;
		}
	}

	BridgeApi& BridgeApi::Get()
	{
		static BridgeApi instance;
		return instance;
	}

	std::uint32_t BridgeApi::GetInterfaceVersion()
	{
		return kBridgeAPIVersion;
	}

	void BridgeApi::GetPluginVersion(std::uint32_t& a_major, std::uint32_t& a_minor, std::uint32_t& a_patch)
	{
		a_major = kPluginVersionMajor;
		a_minor = kPluginVersionMinor;
		a_patch = kPluginVersionPatch;
	}

	const char* BridgeApi::GetBridgeProtocolVersion()
	{
		return kBridgeProtocolVersion;  // static string literal; valid for process lifetime
	}

	bool BridgeApi::IsBridgeReady()
	{
		return _ready.load();
	}

	void BridgeApi::RegisterCommand(const char* a_command, CommandFn a_handler, void* a_user)
	{
		if (!a_command || !a_handler) {
			return;
		}
		const std::string cmd(a_command);
		if (!IsValidPluginCommand(cmd)) {
			REX::WARN("BridgeApi: refused RegisterCommand('{}') — commands are '<author>.<modname>.<name>' "
					  "(two dots minimum; the leading mod id follows the item-1 grammar). "
					  "Single-dot and dotless names are the platform's",
				cmd.substr(0, 128));
			return;
		}
		std::lock_guard lock(_mutex);
		// First-wins (ABI 1.6): a duplicate registration is refused, not
		// last-writer-wins, so an already-claimed command cannot be hijacked.
		// Replacing your own handler means UnregisterCommand then re-register;
		// the pair works back-to-back within one tick.
		if (_commands.contains(cmd)) {
			REX::WARN("BridgeApi: refused RegisterCommand('{}') — already registered (first wins; "
					  "UnregisterCommand first to replace your own handler)",
				cmd);
			return;
		}
		_commands[cmd] = { a_handler, a_user };
		std::erase(_pendingUnregister, cmd);  // cancel a pending removal of the same id
		_dirty = true;
	}

	void BridgeApi::UnregisterCommand(const char* a_command)
	{
		if (!a_command) {
			return;
		}
		const std::string cmd(a_command);
		std::lock_guard lock(_mutex);
		if (_commands.erase(cmd) > 0) {
			_pendingUnregister.push_back(cmd);
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
		const auto parsed = nlohmann::json::parse(a_payloadJson, nullptr, /*allow_exceptions*/ false);
		if (parsed.is_discarded()) {
			return false;
		}
		std::lock_guard lock(_mutex);
		// ABI 1.3: queue even before a bridge is live (older minors returned false
		// here). The pump flushes FIFO once one appears and the renderer stashes
		// per view until the page can receive, so a send issued at plugin load or
		// right before a RequestMenu open is not dropped. Bounded per view so
		// pushes to a view that never comes up can't grow memory unboundedly.
		if (!_ready.load()) {
			std::size_t sameView = 0;
			for (const auto& s : _pendingSends) {
				sameView += (s.view == a_viewId) ? 1u : 0u;
			}
			if (sameView >= kMaxPendingSendsPerView) {
				const auto oldest = std::ranges::find_if(_pendingSends,
					[&](const PendingSend& s) { return s.view == a_viewId; });
				REX::WARN("BridgeApi: pre-ready SendToWeb queue for view '{}' is full ({}); dropping oldest queued '{}'",
					a_viewId, kMaxPendingSendsPerView, oldest->type);
				_pendingSends.erase(oldest);
			}
		}
		_pendingSends.push_back({ std::string(a_viewId), std::string(a_type), std::string(a_payloadJson) });
		return true;
	}

	void BridgeApi::SetReadyCallback(ReadyFn a_callback, void* a_user)
	{
		std::lock_guard lock(_mutex);
		_readyCb = a_callback;
		_readyUser = a_user;
		// If the bridge is already live, re-arm so Pump fires the new callback on
		// the next (main-thread) tick rather than dropping it.
		if (_ready.load()) {
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
		// (Runtime will load it on demand); closes accept only a live surface and
		// never cause an unloaded view to be created.
		if (a_open ? !_knownViews.contains(id) : !_loadedViews.contains(id)) {
			return false;
		}
		_pendingMenuReqs.push_back({ id, a_open });
		return true;
	}

	void BridgeApi::SetViewCatalog(const std::vector<std::string>& a_viewIds)
	{
		std::lock_guard lock(_mutex);
		_knownViews.clear();
		_knownViews.insert(a_viewIds.begin(), a_viewIds.end());
		_loadedViews.clear();
	}

	void BridgeApi::SetSurfaceLoaded(std::string_view a_viewId, bool a_loaded)
	{
		std::lock_guard lock(_mutex);
		if (a_loaded) {
			_loadedViews.emplace(a_viewId);
		} else {
			_loadedViews.erase(std::string(a_viewId));
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
		auto schema = nlohmann::json::parse(a_schemaJson, nullptr, /*allow_exceptions*/ false);
		if (schema.is_discarded()) {
			REX::WARN("BridgeApi: RegisterSettingsSchema rejected — malformed JSON");
			return false;
		}
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

	std::vector<BridgeApi::MenuRequest> BridgeApi::TakeMenuRequests()
	{
		std::lock_guard lock(_mutex);
		std::vector<MenuRequest> out;
		out.swap(_pendingMenuReqs);
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
			REX::WARN("BridgeApi: refused RegisterView('{}') — view ids are qualified "
					  "'<author>.<modname>/<view>' (lowercase [a-z0-9-] segments)",
				std::string_view(a_viewId).substr(0, 128));
			return false;
		}
		// Runtime drains this on the main tick (DrainViewRegistrations), where the
		// manifest lookup and surface registration happen; a not-found id warns
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
		if (!ValidateDiagnosticCaller("ReportIssue", a_modId)) {
			return false;
		}
		if (!a_id || !a_id[0] || !a_code || !a_code[0]) {
			REX::WARN("BridgeApi: refused ReportIssue from '{}' — both an id (the dedupe key) "
					  "and a code (the kind of condition) are required",
				a_modId);
			return false;
		}
		// Context is optional, but a non-object is a producer bug worth reporting
		// synchronously: silently dropping it would leave a card with no detail
		// and no explanation of why.
		nlohmann::json context = nlohmann::json::object();
		if (a_contextJson && a_contextJson[0]) {
			context = nlohmann::json::parse(a_contextJson, nullptr, /*allow_exceptions*/ false);
			if (context.is_discarded() || !context.is_object()) {
				REX::WARN("BridgeApi: refused ReportIssue('{}', '{}') — context must be a JSON object",
					a_modId, a_id);
				return false;
			}
		}
		// Severity is a closed two-value set; a value from a header newer than this
		// host is treated as the worst tier we know, so a future "critical" cannot
		// arrive looking milder than it is.
		if (a_severity > 1u) {
			REX::WARN("BridgeApi: ReportIssue('{}', '{}') — unknown severity {}, treating as error",
				a_modId, a_id, a_severity);
		}

		std::lock_guard lock(_mutex);
		if (_pendingDiagnostics.size() >= kMaxPendingDiagnosticOps) {
			REX::WARN("BridgeApi: refused ReportIssue('{}', '{}') — {} health reports already queued "
					  "for this tick; is a producer reporting in a loop?",
				a_modId, a_id, kMaxPendingDiagnosticOps);
			return false;
		}
		_pendingDiagnostics.push_back(DiagnosticOp{
			.kind = DiagnosticOp::Kind::kReport,
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
		if (!ValidateDiagnosticCaller("ClearIssue", a_modId)) {
			return false;
		}
		if (!a_id || !a_id[0]) {
			return false;
		}
		std::lock_guard lock(_mutex);
		if (_pendingDiagnostics.size() >= kMaxPendingDiagnosticOps) {
			return false;  // warned by the report path; a clear storm implies one
		}
		_pendingDiagnostics.push_back(DiagnosticOp{
			.kind = DiagnosticOp::Kind::kClear,
			.modId = a_modId,
			.id = a_id,
		});
		return true;
	}

	bool BridgeApi::ClearIssuesExcept(const char* a_modId, const char* a_keepIdsJson)
	{
		if (!ValidateDiagnosticCaller("ClearIssuesExcept", a_modId)) {
			return false;
		}
		std::vector<std::string> keep;
		if (a_keepIdsJson && a_keepIdsJson[0]) {
			const auto parsed = nlohmann::json::parse(a_keepIdsJson, nullptr, /*allow_exceptions*/ false);
			if (parsed.is_discarded() || !parsed.is_array()) {
				REX::WARN("BridgeApi: refused ClearIssuesExcept('{}') — keep list must be a JSON array of ids",
					a_modId);
				return false;
			}
			for (const auto& entry : parsed) {
				if (!entry.is_string()) {
					REX::WARN("BridgeApi: refused ClearIssuesExcept('{}') — keep list holds a non-string entry",
						a_modId);
					return false;
				}
				keep.push_back(entry.get<std::string>());
			}
		}
		std::lock_guard lock(_mutex);
		if (_pendingDiagnostics.size() >= kMaxPendingDiagnosticOps) {
			return false;
		}
		_pendingDiagnostics.push_back(DiagnosticOp{
			.kind = DiagnosticOp::Kind::kClearExcept,
			.modId = a_modId,
			.keep = std::move(keep),
		});
		return true;
	}

	std::vector<BridgeApi::DiagnosticOp> BridgeApi::TakeDiagnosticOps()
	{
		std::lock_guard lock(_mutex);
		std::vector<DiagnosticOp> out;
		out.swap(_pendingDiagnostics);
		return out;
	}

	void BridgeApi::OnBridgeReady(MessageBridge* a_bridge)
	{
		std::lock_guard lock(_mutex);
		_bridge = a_bridge;  // a change (incl. null<->ptr) is detected in Pump and forces a re-apply
	}

	void BridgeApi::PumpMainThread()
	{
		// Snapshot the work under the lock, then act unlocked: MessageBridge and
		// the ready callback must not run while holding _mutex — the callback may
		// re-enter our API.
		MessageBridge* bridge = nullptr;
		std::vector<std::string>                      toUnregister;
		std::vector<std::pair<std::string, Registration>> toRegister;
		std::vector<PendingSend>                      sends;
		bool                                          fireReady = false;
		ReadyFn                                       readyCb = nullptr;
		void*                                         readyUser = nullptr;
		{
			std::lock_guard lock(_mutex);
			bridge = _bridge;
			if (bridge) {
				const bool bridgeChanged = (bridge != _appliedBridge);
				if (bridgeChanged || _dirty) {
					if (!bridgeChanged) {
						toUnregister.swap(_pendingUnregister);
					} else {
						_pendingUnregister.clear();  // fresh bridge starts empty — nothing to remove
					}
					toRegister.reserve(_commands.size());
					for (const auto& [cmd, reg] : _commands) {
						toRegister.emplace_back(cmd, reg);
					}
					_appliedBridge = bridge;
					_dirty = false;
				}
				if (!_pendingSends.empty()) {
					sends.swap(_pendingSends);
				}
				if (!_readyFired) {
					_readyFired = true;
					fireReady = true;
					readyCb = _readyCb;
					readyUser = _readyUser;
				}
			}
		}

		if (bridge) {
			for (const auto& cmd : toUnregister) {
				bridge->UnregisterCommand(cmd);
			}
			for (const auto& [cmd, reg] : toRegister) {
				// Trampoline: adapt the ABI-safe CommandFn to the internal handler.
				bridge->RegisterCommand(cmd, [cmd, reg](const nlohmann::json& a_payload, MessageBridge& a_b) {
					// Item-5 envelope: the caller's requestId rides inside the
					// payload JSON (additive — plugins that ignore it lose
					// nothing). After this handler returns the bridge
					// auto-answers `ui.result { ok:true }`, meaning
					// delivered-and-handled. Richer results go out as the
					// plugin's own SendToWeb message, echoing the payload's
					// requestId for correlation.
					std::string dump;
					if (const auto rid = a_b.CurrentRequestId(); !rid.empty()) {
						nlohmann::json withId = a_payload;
						withId["requestId"] = rid;
						dump = withId.dump();
					} else {
						dump = a_payload.dump();
					}
					const std::string src(a_b.CurrentSource());
					reg.fn(cmd.c_str(), dump.c_str(), src.c_str(), reg.user);
				});
			}
			for (const auto& s : sends) {
				bridge->SendJsonToWeb(s.view, s.type, s.payloadJson);
			}
		}

		_ready.store(bridge != nullptr);
		if (fireReady && readyCb) {
			readyCb(readyUser);
		}

		// Settings subscriptions last, so a SubscribeSettings issued from the ready
		// callback above gets its replay this tick, not the next. _subscriptions
		// locks itself and invokes consumer callbacks unlocked; _mutex is not held
		// here.
		_subscriptions.Pump(_mirror);
		// Hotkey fires queued by Runtime::DrainHotkeys earlier this tick; same
		// locking discipline as the settings pump above.
		_hotkeys.Pump();
	}
}
