#pragma once

#include "RE/G/GameMenuBase.h"
#include "RE/S/ScaleformPtr.h"

namespace OSFUI
{
	// Movie-less engine menu whose only job is stack presence: it gives the overlay a slot in the engine's
	// active menu array (open/close bookkeeping, input contexts) and a gamepad capture gate. Cursor and pause
	// are driven separately by FreeCursor and SimPause, so no menu flags are set. Never construct directly;
	// RE::UI invokes Creator on kShow.
	class FocusMenu final : public RE::GameMenuBase
	{
	public:
		SF_MENU_NAME("OSFUI_FocusMenu");

		FocusMenu();

		// IMenu
		const char*   GetName() const override { return MENU_NAME.data(); }
		const char*   GetRootPath() const override { return ""; }  // no movie; empty skips the menuObj bind
		ScaleModeType GetViewScaleMode() override { return ScaleModeType::kNoScale; }
		bool          LoadMovie(bool, bool) override { return true; }  // succeed without a Scaleform movie
		bool          IsMovieLoaded() override { return true; }        // stack-admission predicate; base answers uiMovie != nullptr

		RE::UI_MESSAGE_RESULT ProcessMessage(RE::UIMessageData& a_message) override;

		// GameMenuBase's stack callbacks assume a movie; keep the IMenu base behaviour the movie-less menu was proven on.
		void OnAddedToMenuStack() override { RE::IMenu::OnAddedToMenuStack(); }
		void OnRemovedFromMenuStack() override { RE::IMenu::OnRemovedFromMenuStack(); }

		RE::BSEventNotifyControl ProcessEvent(const RE::UpdateSceneRectEvent&, RE::BSTEventSource<RE::UpdateSceneRectEvent>*) override
		{
			return RE::BSEventNotifyControl::kContinue;
		}

		// BSInputEventUser: accept every event so gamepad input can be stopped here while captured.
		bool ShouldHandleEvent(const RE::InputEvent*) override { return true; }
		void OnThumbstickEvent(const RE::ThumbstickEvent* a_event) override;
		void OnButtonEvent(const RE::ButtonEvent* a_event) override;

		// Platform-facing API; call from the game main thread unless noted.

		// Register idempotently on the first main-thread tick after kPostPostDataLoad.
		static bool Register();

		// Open or close through UIMessageQueue after registration.
		static void Open();
		static void Close();

		// True once Register() has run successfully this session.
		[[nodiscard]] static bool IsRegistered();

		// Engine truth comes from membership in the active menu array.
		[[nodiscard]] static bool IsOpenInEngine();

		// Any thread: while set, gamepad events reaching this menu are stopped (they also route through XInput).
		static void SetGamepadCapture(bool a_capture);

		// Creator handed to RE::UI::RegisterMenu.
		static RE::Scaleform::Ptr<RE::IMenu> Creator();
	};
}
