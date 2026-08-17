#pragma once

namespace OSFUI
{
	class MessageBridge;

	// A feature module ("app") hosted by the runtime. Its purpose is a uniform
	// lifecycle fan-out: the runtime owns a fixed, ordered list of concrete
	// modules and drives each one through the same lifecycle points below —
	// OnStart(), RegisterEndpoints(), OnBridgeDown(), OnViewDestroyed() — from a
	// single loop instead of a per-module call at every site. It is NOT a
	// decoupling seam and NOT a plugin ABI: the runtime still holds and reaches
	// through the concrete module types (currently SettingsModule)
	// directly; this only keeps the lifecycle loops single and in registration
	// order (registration order is meaningful — see BuildModules).
	class IUiModule
	{
	public:
		virtual ~IUiModule() = default;

		// Called once after construction, before the first frame. Apply
		// persisted state / fire startup reactions here. Runs before any
		// bridge-enabled document exists.
		virtual void OnStart() {}

		// Register the module's web-to-native endpoints on the
		// bridge. Called when the runtime bridge is created, independently of which
		// bridge-enabled documents are currently instantiated. A module
		// may keep the bridge pointer for state publication and events until
		// OnBridgeDown (or a later RegisterEndpoints replaces it).
		virtual void RegisterEndpoints(MessageBridge& a_bridge) = 0;

		// The bridge passed to RegisterEndpoints is about to be destroyed; drop
		// any retained pointer or per-document state.
		virtual void OnBridgeDown() {}

		// One view was destroyed (crash-recovery teardown) while the bridge
		// stays up. Drop the id from any per-view tracking state.
		virtual void OnViewDestroyed(std::string_view /*a_viewId*/) {}

		[[nodiscard]] virtual std::string_view Name() const = 0;
	};
}
