#pragma once

namespace OSFUI
{
	class MessageBridge;

	// A self-contained feature ("app") built on the OSF UI platform. The core
	// runtime hosts modules without knowing what any of them does: it calls
	// OnStart() once at load, and RegisterCommands() so the module can wire its
	// own web<->native bridge commands. Settings is the first such module; a
	// HUD, quest tracker, etc. would each be another. This is the seam a future
	// public plugin API would expose so modules can ship in separate DLLs.
	class IUiModule
	{
	public:
		virtual ~IUiModule() = default;

		// Called once after construction, before the first frame. Apply
		// persisted state / fire startup reactions here. Runs even when no
		// bridge-enabled view is active.
		virtual void OnStart() {}

		// Register the module's web<->native `ui.command` handlers on the
		// bridge. Called only when a bridge-enabled view is active. A module
		// may keep the bridge pointer for unsolicited pushes until
		// OnBridgeDown (or a later RegisterCommands replaces it).
		virtual void RegisterCommands(MessageBridge& a_bridge) = 0;

		// The bridge passed to RegisterCommands is about to be destroyed —
		// drop any retained pointer/subscriber state.
		virtual void OnBridgeDown() {}

		[[nodiscard]] virtual std::string_view Name() const = 0;
	};
}
