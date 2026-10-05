#pragma once

#include "RE/G/GameMenuBase.h"
#include "RE/S/ScaleformPtr.h"

namespace OSFUI
{
	// A menu without a movie, used for input contexts and gamepad capture.
	// Cursor and pause belong to FreeCursor/SimPause. RE::UI constructs it via Creator on kShow.
	class FocusMenu final : public RE::GameMenuBase
	{
	public:
		SF_MENU_NAME("OSFUI_FocusMenu");

		FocusMenu();

		const char*   GetName() const override { return MENU_NAME.data(); }
		const char*   GetRootPath() const override { return ""; }  // empty skips the menuObj bind
		ScaleModeType GetViewScaleMode() override { return ScaleModeType::kNoScale; }
		bool          LoadMovie(bool, bool) override { return true; }
		bool          IsMovieLoaded() override { return true; }        // stack-admission predicate; base checks uiMovie

		RE::UI_MESSAGE_RESULT ProcessMessage(RE::UIMessageData& a_message) override;

		// GameMenuBase requires a movie; use IMenu's stack callbacks.
		void OnAddedToMenuStack() override { RE::IMenu::OnAddedToMenuStack(); }
		void OnRemovedFromMenuStack() override { RE::IMenu::OnRemovedFromMenuStack(); }

		RE::BSEventNotifyControl ProcessEvent(const RE::UpdateSceneRectEvent&, RE::BSTEventSource<RE::UpdateSceneRectEvent>*) override
		{
			return RE::BSEventNotifyControl::kContinue;
		}

		// Accept every event so captured gamepad input can be stopped here.
		bool ShouldHandleEvent(const RE::InputEvent*) override { return true; }
		void OnThumbstickEvent(const RE::ThumbstickEvent* a_event) override;
		void OnButtonEvent(const RE::ButtonEvent* a_event) override;

		// Call from the post-menu-advance runtime update unless noted.

		// Idempotent; first runtime tick after kPostPostDataLoad.
		static bool Register();

		// Queued via UIMessageQueue; no-op until registered.
		static void Open();
		static void Close();

		[[nodiscard]] static bool IsRegistered();

		[[nodiscard]] static bool IsOpenInEngine();

		// Any thread. While set, gamepad events reaching this menu are stopped (XInput still sees them).
		static void SetGamepadCapture(bool a_capture);

		static RE::Scaleform::Ptr<RE::IMenu> Creator();
	};
}
