#pragma once

#if defined(OSFUI_WITH_WORLD_SURFACES)

namespace OSFUI
{
	// First interaction slice for world screens. The established WndProc hook
	// reports the initial keyboard-E edge here; the runtime-verified player
	// crosshair field identifies the intended CK Activator without introducing
	// another Address Library relocation.
	class WorldSurfaceActivateSink final
	{
	public:
		static bool Install();
		static void OnKeyDown(std::uint32_t a_virtualKey, bool a_repeat);
	};
}

#endif