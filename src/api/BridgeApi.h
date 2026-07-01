#pragma once

#include "OSFUI_API.h"  // IOSFUIBridge, CommandFn, ReadyFn, version constants (sdk/, on the include path)

namespace OSFUI
{
	class MessageBridge;
}

namespace OSFUI::API
{
	// Concrete IOSFUIBridge singleton — the native-side implementation a sibling
	// SFSE plugin talks to via OSFUI_RequestBridge (src/api/Exports.cpp).
	//
	// It keeps its OWN command registry, independent of MessageBridge lifetime, so
	// a consumer can register before any bridge exists and never re-register: the
	// registry is (re)applied to the live MessageBridge on the main thread whenever
	// it appears or is re-created. All ABI methods are callable from any thread;
	// effects are marshaled onto Runtime::Tick via PumpMainThread. Command/ready
	// callbacks always fire on the main thread. See docs/native-plugin-api.md.
	class BridgeApi final : public IOSFUIBridge
	{
	public:
		[[nodiscard]] static BridgeApi& Get();

		// --- IOSFUIBridge ABI surface (any thread) ---
		std::uint32_t GetInterfaceVersion() override;
		void          GetPluginVersion(std::uint32_t& a_major, std::uint32_t& a_minor, std::uint32_t& a_patch) override;
		const char*   GetBridgeProtocolVersion() override;
		bool          IsBridgeReady() override;
		void          RegisterCommand(const char* a_command, CommandFn a_handler, void* a_user) override;
		void          UnregisterCommand(const char* a_command) override;
		bool          SendToWeb(const char* a_viewId, const char* a_type, const char* a_payloadJson) override;
		void          SetReadyCallback(ReadyFn a_callback, void* a_user) override;

		// --- Runtime wiring (MAIN thread only) ---
		// Hand the live MessageBridge (or nullptr when no nativeBridge view exists)
		// to the API. A different pointer than last time triggers a full re-apply.
		void OnBridgeReady(MessageBridge* a_bridge);
		// Drain queued ops on the main thread: (re)apply the command registry to the
		// live bridge, flush queued sends, fire the ready callback once. Call each tick.
		void PumpMainThread();

	private:
		BridgeApi() = default;
		~BridgeApi() = default;
		BridgeApi(const BridgeApi&) = delete;
		BridgeApi& operator=(const BridgeApi&) = delete;

		struct Registration
		{
			CommandFn fn{ nullptr };
			void*     user{ nullptr };
		};
		struct PendingSend
		{
			std::string view;
			std::string type;
			std::string payloadJson;
		};

		std::mutex                                    _mutex;
		std::unordered_map<std::string, Registration> _commands;          // desired command set
		std::vector<std::string>                      _pendingUnregister;  // to remove from a live bridge
		std::vector<PendingSend>                       _pendingSends;
		MessageBridge*                                _bridge{ nullptr };         // non-owning; set on main thread
		MessageBridge*                                _appliedBridge{ nullptr };  // bridge we last applied to
		bool                                          _dirty{ false };            // command set changed since apply
		ReadyFn                                       _readyCb{ nullptr };
		void*                                         _readyUser{ nullptr };
		bool                                          _readyFired{ false };
		std::atomic_bool                              _ready{ false };            // IsBridgeReady() fast path
	};
}
