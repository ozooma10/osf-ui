#include "api/BridgeApi.h"

#include "core/Version.h"
#include "runtime/MessageBridge.h"

namespace OSFUI::API
{
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
		const REL::Version v = SFSE::GetPluginVersion();
		a_major = v.major();
		a_minor = v.minor();
		a_patch = v.patch();
	}

	const char* BridgeApi::GetBridgeProtocolVersion()
	{
		return kBridgeProtocolVersion;
	}

	bool BridgeApi::IsBridgeReady()
	{
		return _ready.load();
	}

	bool BridgeApi::IsReservedPrefix(std::string_view a_command)
	{
		// Platform / first-party namespaces a consumer may not claim
		return a_command.starts_with("ui.") ||
		       a_command.starts_with("runtime.") ||
		       a_command.starts_with("game.") ||
		       a_command.starts_with("settings.");
	}

	void BridgeApi::RegisterCommand(const char* a_command, CommandFn a_handler, void* a_user)
	{
		if (!a_command || !a_handler) {
			REX::WARN("BridgeApi: RegisterCommand ignored — null command or handler");
			return;
		}
		std::string command(a_command);
		if (IsReservedPrefix(command)) {
			REX::WARN("BridgeApi: refusing to register reserved command '{}' (ui./runtime./game./settings. are platform-owned)", command);
			return;
		}

		std::scoped_lock lock(_mutex);
		if (_commands.contains(command)) {
			REX::WARN("BridgeApi: command '{}' re-registered — last write wins", command);
		}
		_commands[command] = { a_handler, a_user };
		_ops.push_back({ Op::Kind::Register, command, { a_handler, a_user } });
	}

	void BridgeApi::UnregisterCommand(const char* a_command)
	{
		if (!a_command) {
			return;
		}
		std::string command(a_command);

		std::scoped_lock lock(_mutex);
		if (_commands.erase(command) > 0) {
			_ops.push_back({ Op::Kind::Unregister, command, {} });
		}
	}

	bool BridgeApi::SendToWeb(const char* a_viewId, const char* a_type, const char* a_payloadJson)
	{
		if (!a_viewId || !a_type || !a_payloadJson) {
			return false;
		}
		if (!_ready.load()) {
			return false;  // no live bridge yet — push from the ready callback instead
		}
		// Validate synchronously so the caller learns about malformed JSON now, not silently on a later frame.
		auto payload = nlohmann::json::parse(a_payloadJson, nullptr, /*allow_exceptions*/ false);
		if (payload.is_discarded()) {
			return false;
		}

		std::scoped_lock lock(_mutex);
		_pendingSends.push_back({ std::string(a_viewId), std::string(a_type), std::move(payload) });
		return true;
	}

	void BridgeApi::SetReadyCallback(ReadyFn a_callback, void* a_user)
	{
		std::scoped_lock lock(_mutex);
		_readyCb = a_callback;
		_readyUser = a_user;
		// Registered late (bridge already live)? Fire once on the next main tick.
		if (a_callback && _ready.load()) {
			_firePendingReady = true;
		}
	}

	void BridgeApi::ApplyCommand(const std::string& a_command, const Registration& a_reg)
	{
		if (!_bridge) {
			return;
		}
		const CommandFn fn = a_reg.fn;
		void* const     user = a_reg.user;
		// Trampoline: adapt the internal CommandHandler (nlohmann::json) to the ABI-safe CommandFn (JSON text). 
		// Runs on the main thread, like every MessageBridge handler.
		_bridge->RegisterCommand(a_command,
			[a_command, fn, user](const nlohmann::json& a_payload, MessageBridge& a_b) {
				const std::string dump = a_payload.dump();
				const std::string src(a_b.CurrentSource());
				fn(a_command.c_str(), dump.c_str(), src.c_str(), user);
			});
	}

	void BridgeApi::OnBridgeReady(MessageBridge* a_bridge)
	{
		// Main thread (Runtime::Initialize / Shutdown).
		std::unordered_map<std::string, Registration> snapshot;
		ReadyFn                                       readyCb = nullptr;
		void*                                         readyUser = nullptr;
		{
			std::scoped_lock lock(_mutex);
			_bridge = a_bridge;
			_ops.clear();  // the full (re)apply below subsumes any queued command deltas
			if (!a_bridge) {
				_ready.store(false);
				_pendingSends.clear();  // nothing left to deliver to
				return;
			}
			snapshot = _commands;
			readyCb = _readyCb;
			readyUser = _readyUser;
			_firePendingReady = false;  // fired below; don't double-fire in the pump
		}

		for (const auto& [command, reg] : snapshot) {
			ApplyCommand(command, reg);
		}
		_ready.store(true);
		REX::INFO("BridgeApi: bridge ready — applied {} registered command(s)", snapshot.size());
		if (readyCb) {
			readyCb(readyUser);
		}
	}

	void BridgeApi::PumpMainThread()
	{
		// Main thread (Runtime::Tick). Without a live bridge, deferred command ops wait for OnBridgeReady (which applies them from _commands); 
		if (!_bridge) {
			return;
		}

		std::vector<Op>          ops;
		std::vector<PendingSend> sends;
		ReadyFn                  readyCb = nullptr;
		void*                    readyUser = nullptr;
		bool                     fireReady = false;
		{
			std::scoped_lock lock(_mutex);
			ops.swap(_ops);
			sends.swap(_pendingSends);
			if (_firePendingReady) {
				fireReady = true;
				readyCb = _readyCb;
				readyUser = _readyUser;
				_firePendingReady = false;
			}
		}

		// Lock released before touching the bridge or consumer callbacks: those may call back into RegisterCommand/SendToWeb (which take _mutex).
		for (const auto& op : ops) {
			if (op.kind == Op::Kind::Register) {
				ApplyCommand(op.command, op.reg);
			} else {
				_bridge->UnregisterCommand(op.command);
			}
		}
		for (const auto& send : sends) {
			_bridge->SendToWeb(send.view, send.type, send.payload);
		}
		if (fireReady && readyCb) {
			readyCb(readyUser);
		}
	}
}
