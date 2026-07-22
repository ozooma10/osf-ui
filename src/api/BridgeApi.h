#pragma once

#include <unordered_set>

#include "OSFUI_API.h"  // IOSFUIBridge, CommandFn, ReadyFn, version constants (sdk/, on the include path)

#include "api/HotkeySubscriptions.h"
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
	// Keeps its own command registry, independent of MessageBridge lifetime, so a
	// consumer can register before any bridge exists and never re-register: the
	// registry is (re)applied to the live MessageBridge on the main thread whenever
	// it appears or is re-created. All ABI methods are callable from any thread;
	// effects are marshaled onto Runtime::Tick via PumpMainThread. Command/ready
	// callbacks fire on the main thread. See docs/native-plugin-api.md.
	class BridgeApi final : public IOSFUIBridge
	{
	public:
		[[nodiscard]] static BridgeApi& Get();

		// IOSFUIBridge ABI surface (any thread).
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
		bool          RegisterSettingsSchema(const char* a_schemaJson) override;
		void          UnregisterSettingsSchema(const char* a_modId) override;
		std::uint32_t SubscribeHotkey(const char* a_modId, const char* a_key, HotkeyFn a_fn, void* a_user) override;
		void          UnsubscribeHotkey(std::uint32_t a_token) override;
		bool          RegisterView(const char* a_viewId) override;

		// Runtime wiring (main thread only).
		// A menu open/close a sibling plugin requested via RequestMenu.
		struct MenuRequest
		{
			std::string view;
			bool        open{ true };
		};
		// Drain the queued menu requests. Runtime snapshots these at the top of
		// Tick and applies each through its own menu policy (_menus.Open/Close +
		// ApplyMenuPolicy) after PumpMainThread, which is what guarantees
		// SendToWeb lands before RequestMenu.
		std::vector<MenuRequest> TakeMenuRequests();

		// Install the boot discovery catalog used by RequestMenu's synchronous
		// existence check, and mirror surface load/unload transitions for close
		// validation. Runtime owns the catalog; these copies are protected by the
		// API mutex because RequestMenu is callable from any thread.
		void SetViewCatalog(const std::vector<std::string>& a_viewIds);
		void SetSurfaceLoaded(std::string_view a_viewId, bool a_loaded);

		// A queued RegisterSettingsSchema (schema is an object) or
		// UnregisterSettingsSchema (schema is null, modId set) — already
		// shape-validated synchronously; FIFO so register-then-unregister of
		// the same id lands in call order.
		struct SchemaOp
		{
			nlohmann::json schema;
			std::string    modId;
		};
		// Drain the queued schema ops. Runtime applies each to the SettingsStore
		// (Source::kNative) in DrainSchemaOps.
		std::vector<SchemaOp> TakeSchemaOps();

		// Drain the queued RegisterView ids. Runtime loads + surface-registers each
		// in DrainViewRegistrations, before the menu request snapshot, so
		// RegisterView -> SendToWeb -> RequestMenu issued back-to-back land in one
		// tick (ABI 1.5).
		std::vector<std::string> TakeViewRegistrations();

		// The any-thread settings value mirror the ABI typed getters read
		// (mcm-design.md §8.2). Runtime::BuildModules feeds it from the store's
		// change/registry listeners on the main thread; the getters (and the
		// Papyrus natives) read it from any thread.
		[[nodiscard]] SettingsMirror& Mirror() { return _mirror; }

		// SubscribeSettings bookkeeping (mcm-design.md §8.2). Runtime's store
		// change listener feeds OnChanged (right after Mirror().Update, main
		// thread); PumpMainThread drains replays + queued changes each tick.
		[[nodiscard]] SettingsSubscriptions& Subscriptions() { return _subscriptions; }

		// SubscribeHotkey bookkeeping (mcm-design.md §9). Runtime::DrainHotkeys
		// feeds OnFired (main thread); PumpMainThread drains the queue each tick.
		[[nodiscard]] HotkeySubscriptions& Hotkeys() { return _hotkeys; }

		// Hand the live MessageBridge (or nullptr when no nativeBridge view exists)
		// to the API. A different pointer than last time triggers a full re-apply.
		void OnBridgeReady(MessageBridge* a_bridge);
		// Main thread; call each tick. (Re)applies the command registry to the live
		// bridge, flushes queued sends, fires the ready callback once.
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
		HotkeySubscriptions                           _hotkeys;           // own locking; never touched under _mutex
		std::unordered_map<std::string, Registration> _commands;          // desired command set
		std::vector<std::string>                      _pendingUnregister;  // to remove from a live bridge
		std::vector<PendingSend>                       _pendingSends;
		std::vector<MenuRequest>                      _pendingMenuReqs;    // RequestMenu ops, drained by Runtime
		std::unordered_set<std::string>               _knownViews;         // boot-discovered manifest ids
		std::unordered_set<std::string>               _loadedViews;        // currently registered renderer surfaces
		std::vector<SchemaOp>                         _pendingSchemaOps;   // schema (un)registrations, drained by Runtime
		std::vector<std::string>                      _pendingViewRegs;    // RegisterView ids, drained by Runtime
		MessageBridge*                                _bridge{ nullptr };         // non-owning; set on main thread
		MessageBridge*                                _appliedBridge{ nullptr };  // bridge we last applied to
		bool                                          _dirty{ false };            // command set changed since apply
		ReadyFn                                       _readyCb{ nullptr };
		void*                                         _readyUser{ nullptr };
		bool                                          _readyFired{ false };
		std::atomic_bool                              _ready{ false };            // IsBridgeReady() fast path
	};
}
