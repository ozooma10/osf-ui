#include "Input/HardwareCursor.h"

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
		// The renderer callback publishes the system cursor ID; WndProc applies it.
		std::atomic<std::uint32_t> g_systemCursorId{ 0 };

		// Window-message thread only.
		bool g_active{ false };
		int  g_showRaises{ 0 };  // net ShowCursor(TRUE) calls, undone on Deactivate
		HCURSOR g_previousCursor{ nullptr };
		HCURSOR g_appliedCursor{ nullptr };

		// Bound ShowCursor raises in case another owner holds visibility.
		constexpr int kMaxShowRaises = 8;

		[[nodiscard]] bool PointerShowing()
		{
			CURSORINFO info{ .cbSize = sizeof(CURSORINFO) };
			return ::GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING) != 0;
		}

		void RaiseUntilShowing()
		{
			if (PointerShowing()) return;
			while (g_showRaises < kMaxShowRaises) {
				const int count = ::ShowCursor(TRUE);
				++g_showRaises;
				if (count >= 0) break;
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
		g_previousCursor = ::GetCursor();

		// A null cursor stays invisible regardless of the ShowCursor count.
		ApplyShape();
		RaiseUntilShowing();

		RECT screen{};
		if (ClientRectOnScreen(hwnd, screen)) {
			::SetCursorPos((screen.left + screen.right) / 2, (screen.top + screen.bottom) / 2);
			::ClipCursor(&screen);
		}
		REX::DEBUG("HardwareCursor: activated (showRaises={}, centered + clipped to client)", g_showRaises);
	}

	void Deactivate()
	{
		if (!g_active) {
			return;
		}
		g_active = false;
		// Restore a null (hidden) game cursor too, unless another owner already replaced ours.
		if (::GetCursor() == g_appliedCursor) {
			::SetCursor(g_previousCursor);
		}
		g_previousCursor = nullptr;
		g_appliedCursor = nullptr;
		for (; g_showRaises > 0; --g_showRaises) {
			::ShowCursor(FALSE);
		}
		::ClipCursor(nullptr);
		REX::DEBUG("HardwareCursor: deactivated (visibility + clip returned to the game)");
	}

	bool IsActive()
	{
		return g_active;
	}

	void Reassert(void* a_hwnd)
	{
		if (!g_active) {
			return;
		}
		ApplyShape();
		RaiseUntilShowing();
		// Restore cursor clipping after engine or resolution changes.
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
		const auto id = g_systemCursorId.load(std::memory_order_relaxed);
		// Resource ordinals are 16-bit; do not truncate an invalid incoming ID.
		auto cursor = id <= 0xFFFF ? ::LoadCursorW(nullptr, MAKEINTRESOURCEW(id)) : nullptr;
		// WebView2 reports 0 for custom CSS cursors; fall back to the arrow. Shared handles: never destroyed.
		if (!cursor) {
			cursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
		}
		g_appliedCursor = cursor;
		::SetCursor(cursor);
	}

	void SetSystemCursorId(std::uint32_t a_id)
	{
		g_systemCursorId.store(a_id, std::memory_order_relaxed);
	}
}
