#include "input/HardwareCursor.h"

#include <atomic>

// Keep <Windows.h> confined to this file.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

namespace OSFUI::HardwareCursor
{
	namespace
	{
		// Written by SetShape (renderer worker thread), read on the window-message
		// thread when applying.
		std::atomic<CursorShape> g_shape{ CursorShape::kArrow };

		// Window-message thread only.
		bool g_active{ false };
		int  g_showRaises{ 0 };  // net ShowCursor(TRUE) calls, undone on Deactivate

		// The game may have hidden the pointer several counter-levels deep; cap
		// the raises in case visibility is held by something we shouldn't override.
		constexpr int kMaxShowRaises = 8;

		[[nodiscard]] HCURSOR SystemCursor(CursorShape a_shape)
		{
			// IDC_* are integer resource ordinals; this build is not UNICODE, so
			// they expand to LPSTR — use the A variant (identical cursors).
			LPCSTR id = IDC_ARROW;
			switch (a_shape) {
			case CursorShape::kCross:      id = IDC_CROSS; break;
			case CursorShape::kHand:       id = IDC_HAND; break;
			case CursorShape::kIBeam:      id = IDC_IBEAM; break;
			case CursorShape::kWait:       id = IDC_WAIT; break;
			case CursorShape::kHelp:       id = IDC_HELP; break;
			case CursorShape::kNotAllowed: id = IDC_NO; break;
			case CursorShape::kSizeWE:     id = IDC_SIZEWE; break;
			case CursorShape::kSizeNS:     id = IDC_SIZENS; break;
			case CursorShape::kSizeNESW:   id = IDC_SIZENESW; break;
			case CursorShape::kSizeNWSE:   id = IDC_SIZENWSE; break;
			case CursorShape::kSizeAll:    id = IDC_SIZEALL; break;
			default:                       break;  // kArrow (kNone never reaches here)
			}
			// Shared system handle: not destroyed, cheap to look up.
			return ::LoadCursorA(nullptr, id);
		}

		[[nodiscard]] bool PointerShowing()
		{
			CURSORINFO info{ .cbSize = sizeof(CURSORINFO) };
			return ::GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING) != 0;
		}

		void RaiseUntilShowing()
		{
			while (g_showRaises < kMaxShowRaises && !PointerShowing()) {
				::ShowCursor(TRUE);
				++g_showRaises;
			}
		}

		[[nodiscard]] bool ClientRectOnScreen(HWND a_hwnd, RECT& a_out)
		{
			RECT rc{};
			if (!::GetClientRect(a_hwnd, &rc)) {
				return false;
			}
			POINT tl{ rc.left, rc.top };
			POINT br{ rc.right, rc.bottom };
			::ClientToScreen(a_hwnd, &tl);
			::ClientToScreen(a_hwnd, &br);
			a_out = RECT{ tl.x, tl.y, br.x, br.y };
			return true;
		}
	}

	void Activate(void* a_hwnd)
	{
		if (g_active) {
			return;
		}
		g_active = true;
		const auto hwnd = static_cast<HWND>(a_hwnd);

		RaiseUntilShowing();
		ApplyShape();

		RECT screen{};
		if (ClientRectOnScreen(hwnd, screen)) {
			::SetCursorPos((screen.left + screen.right) / 2, (screen.top + screen.bottom) / 2);
			::ClipCursor(&screen);
		}
		REX::INFO("HardwareCursor: activated (showRaises={}, centered + clipped to client)", g_showRaises);
	}

	void Deactivate()
	{
		if (!g_active) {
			return;
		}
		g_active = false;
		for (; g_showRaises > 0; --g_showRaises) {
			::ShowCursor(FALSE);
		}
		::ClipCursor(nullptr);
		REX::INFO("HardwareCursor: deactivated (visibility + clip returned to the game)");
	}

	void Reassert(void* a_hwnd)
	{
		if (!g_active) {
			return;
		}
		RaiseUntilShowing();
		ApplyShape();
		// Re-fence if the engine re-clipped (or a resolution change moved the
		// client area). Compare against the live clip (::GetClipCursor below):
		// the engine changing it out from under us is the case to heal.
		RECT want{};
		if (!ClientRectOnScreen(static_cast<HWND>(a_hwnd), want)) {
			return;
		}
		RECT current{};
		if (!::GetClipCursor(&current) ||
			current.left != want.left || current.top != want.top ||
			current.right != want.right || current.bottom != want.bottom) {
			::ClipCursor(&want);
		}
	}

	void ApplyShape()
	{
		const auto shape = g_shape.load(std::memory_order_relaxed);
		// kNone = the page asked to hide the pointer (CSS `cursor: none`); a null
		// cursor hides it without touching the show counter.
		::SetCursor(shape == CursorShape::kNone ? nullptr : SystemCursor(shape));
	}

	void SetShape(CursorShape a_shape)
	{
		g_shape.store(a_shape, std::memory_order_relaxed);
	}
}
