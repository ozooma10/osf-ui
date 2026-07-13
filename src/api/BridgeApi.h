#pragma once

#include "OSFUI_API.h"  // IOSFUIBridge, CommandFn, ReadyFn, version constants (sdk/, on the include path)

#include "api/SettingsMirror.h"
#include "api/SettingsSubscriptions.h"

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
		bool          RequestMenu(const char* a_viewId, bool a_open) override;
		std::uint32_t SubscribeSettings(const char* a_modId, SettingChangedFn a_fn, void* a_user) override;
		void          UnsubscribeSettings(std::uint32_t a_token) override;
		bool          GetSettingBool(const char* a_modId, const char* a_key, bool* a_out) override;
		bool          GetSettingInt(const char* a_modId, const char* a_key, std::int64_t* a_out) override;
		bool          GetSettingFloat(const char* a_modId, const char* a_key, double* a_out) override;
		std::uint32_t GetSettingString(const char* a_modId, const char* a_key, char* a_buf, std::uint32_t a_bufLen) override;

		// --- Runtime wiring (MAIN thread only) ---
		// A menu open/close a sibling plugin requested via RequestMenu.
		struct MenuRequest
		{
			std::string view;
			bool        open{ true };
		};
		// Drain the queued menu requests (MAIN thread). Runtime applies each through its own menu policy (_menus.Open/Close + ApplyMenuPolicy) in DrainMenuRequests.
		std::vector<MenuRequest> TakeMenuRequests();

		// The any-thread settings value mirror the ABI typed getters read
		// (mcm-design.md §8.2). Runtime::BuildModules feeds it from the store's
		// change/registry listeners on the MAIN thread; the getter methods (and
		// later Papyrus natives) read it from any thread.
		[[nodiscard]] SettingsMirror& Mirror() { return _mirror; }

		// SubscribeSettings bookkeeping (mcm-design.md §8.2). Runtime's store
		// change listener feeds OnChanged (right after Mirror().Update, MAIN
		// thread); PumpMainThread drains replays + queued changes each tick.
		[[nodiscard]] SettingsSubscriptions& Subscriptions() { return _subscriptions; }

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
		SettingsMirror                                _mirror;            // own locking; never touched under _mutex
		SettingsSubscriptions                         _subscriptions;     // own locking; never touched under _mutex
		std::unordered_map<std::string, Registration> _commands;          // desired command set
		std::vector<std::string>                      _pendingUnregister;  // to remove from a live bridge
		std::vector<PendingSend>                       _pendingSends;
		std::vector<MenuRequest>                      _pendingMenuReqs;    // RequestMenu ops, drained by Runtime
		MessageBridge*                                _bridge{ nullptr };         // non-owning; set on main thread
		MessageBridge*                                _appliedBridge{ nullptr };  // bridge we last applied to
		bool                                          _dirty{ false };            // command set changed since apply
		ReadyFn                                       _readyCb{ nullptr };
		void*                                         _readyUser{ nullptr };
		bool                                          _readyFired{ false };
		std::atomic_bool                              _ready{ false };            // IsBridgeReady() fast path
	};
}
