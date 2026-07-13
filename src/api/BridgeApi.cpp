#include "api/BridgeApi.h"

#include "core/Version.h"
#include "runtime/MessageBridge.h"  // also pulls nlohmann/json
#include "runtime/SettingsStore.h"  // ValidateSchemaShape — the synchronous shape gate

namespace OSFUI::API
{
	namespace
	{
		// Platform / first-party namespaces a consumer may not register into.
		bool IsReservedCommand(const std::string& a_cmd)
		{
			return a_cmd.starts_with("ui.") || a_cmd.starts_with("runtime.") ||
			       a_cmd.starts_with("game.") || a_cmd.starts_with("settings.") ||
			       a_cmd.starts_with("views.");
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
		if (IsReservedCommand(cmd)) {
			REX::WARN("BridgeApi: refused RegisterCommand('{}') — reserved prefix (ui./runtime./game./settings./views.)", cmd);
			return;
		}
		std::lock_guard lock(_mutex);
		if (_commands.contains(cmd)) {
			REX::WARN("BridgeApi: command '{}' re-registered — last writer wins", cmd);
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
		if (!_ready.load()) {
			return false;  // no live bridge yet — consumer should send from the ready callback
		}
		// Validate now so a malformed payload is reported synchronously; delivery is
		// marshaled to the main thread in PumpMainThread.
		const auto parsed = nlohmann::json::parse(a_payloadJson, nullptr, /*allow_exceptions*/ false);
		if (parsed.is_discarded()) {
			return false;
		}
		std::lock_guard lock(_mutex);
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
		// Queue it like a send; Runtime drains it on the main tick and runs it through the normal menu policy. Requesting an open before any bridge is live is fine
		// the request waits and Runtime applies it once a surface can be shown.
		std::lock_guard lock(_mutex);
		_pendingMenuReqs.push_back({ std::string(a_viewId), a_open });
		return true;
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

	bool BridgeApi::RegisterSettingsSchema(const char* a_schemaJson)
	{
		if (!a_schemaJson) {
			return false;
		}
		// Parse + shape errors report synchronously (the ABI contract); the
		// store merge itself is marshaled to the main tick (Runtime::
		// DrainSchemaOps), where precedence resolves with a log warning.
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

	void BridgeApi::OnBridgeReady(MessageBridge* a_bridge)
	{
		std::lock_guard lock(_mutex);
		_bridge = a_bridge;  // a change (incl. null<->ptr) is detected in Pump and forces a re-apply
	}

	void BridgeApi::PumpMainThread()
	{
		// Snapshot the work under the lock, then act unlocked — the actions call
		// into MessageBridge and the ready callback, which must never run while
		// holding _mutex (the callback may re-enter our API).
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
					const std::string dump = a_payload.dump();
					const std::string src(a_b.CurrentSource());
					reg.fn(cmd.c_str(), dump.c_str(), src.c_str(), reg.user);
				});
			}
			for (const auto& s : sends) {
				const auto payload = nlohmann::json::parse(s.payloadJson, nullptr, /*allow_exceptions*/ false);
				if (!payload.is_discarded()) {
					bridge->SendToWeb(s.view, s.type, payload);
				}
			}
		}

		_ready.store(bridge != nullptr);
		if (fireReady && readyCb) {
			readyCb(readyUser);
		}

		// Settings subscriptions last, so a SubscribeSettings issued from the
		// ready callback above gets its replay THIS tick, not the next.
		// _subscriptions locks itself and invokes consumer callbacks unlocked;
		// _mutex is not held here.
		_subscriptions.Pump(_mirror);
	}
}
