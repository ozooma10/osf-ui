#pragma once

#include "RE/I/IMenu.h"
#include "RE/S/ScaleformPtr.h"

namespace OSFUI
{
	// RE 1.16.244: Creator builds the movie-less engine IMenu with copied RTTI/vtable; never construct FocusMenu directly.
	class FocusMenu final
	{
	public:
		static constexpr std::string_view MENU_NAME = "OSFUI_FocusMenu";

		// Platform-facing API; call from the game main thread.

		// Register idempotently on the first main-thread tick after kPostPostDataLoad.
		static bool Register();

		// Main-thread only: open or close through UIMessageQueue after registration.
		static void Open();
		static void Close();

		// True once Register() has run successfully this session.
		[[nodiscard]] static bool IsRegistered();

		// Main-thread engine truth comes from membership in the admitted menu array.
		[[nodiscard]] static bool IsOpenInEngine();

		// Worker-safe capture policy stops gamepad events that also route through XInput.
		static void SetGamepadCapture(bool a_capture);

		// Creator handed to RE::UI::RegisterMenu (UIMenuEntry::Create_t).
		static RE::Scaleform::Ptr<RE::IMenu>* Creator(RE::Scaleform::Ptr<RE::IMenu>* a_out);
	};
}
