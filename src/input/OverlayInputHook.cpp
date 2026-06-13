#include "input/OverlayInputHook.h"

#include "runtime/Runtime.h"

// Keep <Windows.h> confined to this file. NOGDI stops wingdi's ERROR macro
// from clobbering REX::ERROR.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

namespace SWUI::OverlayInputHook
{
	namespace
	{
		WNDPROC g_originalProc{ nullptr };
		HWND    g_hwnd{ nullptr };

		struct FindWindowData
		{
			DWORD pid{ 0 };
			HWND  best{ nullptr };
		};

		BOOL CALLBACK EnumProc(HWND a_hwnd, LPARAM a_param)
		{
			auto* data = reinterpret_cast<FindWindowData*>(a_param);
			DWORD wndPid = 0;
			::GetWindowThreadProcessId(a_hwnd, &wndPid);
			if (wndPid != data->pid) {
				return TRUE;  // keep enumerating
			}
			// Want the visible, top-level (unowned) main window.
			if (!::IsWindowVisible(a_hwnd) || ::GetWindow(a_hwnd, GW_OWNER) != nullptr) {
				return TRUE;
			}
			data->best = a_hwnd;
			return FALSE;  // good enough; stop
		}

		[[nodiscard]] HWND FindGameWindow()
		{
			FindWindowData data{ .pid = ::GetCurrentProcessId(), .best = nullptr };
			::EnumWindows(&EnumProc, reinterpret_cast<LPARAM>(&data));
			return data.best;
		}

		[[nodiscard]] bool IsMouseOrRawMessage(const UINT a_msg)
		{
			switch (a_msg) {
			case WM_INPUT:
			case WM_MOUSEMOVE:
			case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
			case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
			case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
			case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
			case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
				return true;
			default:
				return false;
			}
		}

		LRESULT CALLBACK WndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wparam, LPARAM a_lparam)
		{
			auto& runtime = Runtime::Get();

			switch (a_msg) {
			case WM_KEYDOWN:
			case WM_SYSKEYDOWN:
			{
				const auto vk = static_cast<std::uint32_t>(a_wparam);
				const bool repeat = (a_lparam & 0x40000000) != 0;
				// Drive toggle/web-routing on the initial press only so key
				// auto-repeat can't re-toggle the overlay.
				const bool consume = repeat ? runtime.IsInputCaptured() : runtime.OnHostKey(vk, true);
				if (consume) {
					return 0;
				}
				break;
			}
			case WM_KEYUP:
			case WM_SYSKEYUP:
			{
				const auto vk = static_cast<std::uint32_t>(a_wparam);
				if (runtime.OnHostKey(vk, false)) {
					return 0;
				}
				break;
			}
			case WM_CHAR:
			case WM_UNICHAR:
			case WM_DEADCHAR:
				// Keyboard routing uses the VK stream (WM_KEYDOWN); just block
				// these from the game while captured.
				if (runtime.IsInputCaptured()) {
					return 0;
				}
				break;
			default:
				if (IsMouseOrRawMessage(a_msg) && runtime.IsInputCaptured()) {
					// Block the game's mouse/raw input so the camera freezes.
					// WM_INPUT must still be handed to DefWindowProc to release
					// the raw input buffer (the game's proc is what we skip).
					return (a_msg == WM_INPUT)
					           ? ::DefWindowProcW(a_hwnd, a_msg, a_wparam, a_lparam)
					           : 0;
				}
				break;
			}

			return ::CallWindowProcW(g_originalProc, a_hwnd, a_msg, a_wparam, a_lparam);
		}
	}

	bool Install()
	{
		if (g_originalProc) {
			return true;  // already installed (one-way)
		}

		g_hwnd = FindGameWindow();
		if (!g_hwnd) {
			REX::ERROR("OverlayInputHook: could not find the game window; input capture unavailable");
			return false;
		}

		g_originalProc = reinterpret_cast<WNDPROC>(
			::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
		if (!g_originalProc) {
			REX::ERROR("OverlayInputHook: SetWindowLongPtr failed (Win32 error {})", ::GetLastError());
			return false;
		}

		REX::INFO("OverlayInputHook: subclassed game WndProc (hwnd 0x{:X}); overlay can now capture input",
			reinterpret_cast<std::uintptr_t>(g_hwnd));
		return true;
	}
}
