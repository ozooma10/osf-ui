#include "Input/OverlayInputHook.h"

#include "Core/Log.h"
#include "Input/HardwareCursor.h"
#include "Input/ScanCode.h"
#include "Platform/WindowsPlatform.h"
#include "Runtime/Runtime.h"

// Keep <Windows.h> here with NOGDI to avoid wingdi's ERROR macro.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

namespace OSFUI::OverlayInputHook
{
	namespace
	{
		WNDPROC g_originalProc{ nullptr };
		WNDPROC g_gameProc{ nullptr };
		HWND    g_hwnd{ nullptr };
		std::atomic_bool g_chainCycleLogged{ false };

		// Normalize WM key identity to DIK, falling back from absent synthetic scan codes to VK mapping.
		ScanCode MessageScanCode(std::uint32_t a_vk, LPARAM a_lparam)
		{
			const auto rawScan = static_cast<std::uint8_t>((a_lparam >> 16) & 0xFF);
			const bool extended = (a_lparam & 0x01000000) != 0;
			const auto scan = ComposeScanCode(a_vk, rawScan, extended);
			if (scan != kInvalidScanCode) {
				return scan;
			}
			return static_cast<ScanCode>(Platform::VkToDirectInputScan(a_vk));
		}
		thread_local bool g_forwardingOriginal{ false };

		// Window-thread cursor state observes capture edges published by the main thread.
		bool g_hwCursorActive{ false };

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

		[[nodiscard]] bool IsLegacyMouseMessage(const UINT a_msg)
		{
			switch (a_msg) {
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

		// Route WM_INPUT using the visible OS pointer; legacy mouse messages are suppressed by the game.
		void RouteRawMouse(HWND a_hwnd, LPARAM a_lparam)
		{
			UINT size = 0;
			if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(a_lparam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 ||
				size == 0 || size > sizeof(RAWINPUT)) {
				return;
			}
			RAWINPUT raw{};
			if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(a_lparam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != size ||
				raw.header.dwType != RIM_TYPEMOUSE) {
				return;
			}

			auto& runtime = Runtime::Get();
			const auto& mouse = raw.data.mouse;

			// Heal engine cursor changes on the next visible input packet.
			HardwareCursor::Reassert(a_hwnd);
			// Sync every packet so clicks without prior movement land correctly.
			POINT pt{};
			RECT  client{};
			if (::GetCursorPos(&pt) && ::ScreenToClient(a_hwnd, &pt) &&
				::GetClientRect(a_hwnd, &client) && client.right > 0 && client.bottom > 0) {
				runtime.OnGameWindowMouseAbsolute(pt.x, pt.y, client.right, client.bottom);
			}

			const auto buttons = mouse.usButtonFlags;
			if (buttons & RI_MOUSE_LEFT_BUTTON_DOWN) {
				runtime.OnGameWindowMouseButton(0, true);
			}
			if (buttons & RI_MOUSE_LEFT_BUTTON_UP) {
				runtime.OnGameWindowMouseButton(0, false);
			}
			if (buttons & RI_MOUSE_RIGHT_BUTTON_DOWN) {
				runtime.OnGameWindowMouseButton(1, true);
			}
			if (buttons & RI_MOUSE_RIGHT_BUTTON_UP) {
				runtime.OnGameWindowMouseButton(1, false);
			}
			if (buttons & RI_MOUSE_MIDDLE_BUTTON_DOWN) {
				runtime.OnGameWindowMouseButton(2, true);
			}
			if (buttons & RI_MOUSE_MIDDLE_BUTTON_UP) {
				runtime.OnGameWindowMouseButton(2, false);
			}

			// Reinterpret unsigned usButtonData as signed WHEEL_DELTA units.
			if (buttons & RI_MOUSE_WHEEL) {
				const auto wheelDelta = static_cast<short>(mouse.usButtonData);
				if (wheelDelta != 0) {
					runtime.OnGameWindowMouseWheel(static_cast<int>(wheelDelta));
				}
			}
		}

		LRESULT CALLBACK WndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wparam, LPARAM a_lparam);

		LRESULT ForwardToGame(HWND a_hwnd, UINT a_msg, WPARAM a_wparam, LPARAM a_lparam)
		{
			if (g_gameProc && g_gameProc != &WndProc) {
				return ::CallWindowProcW(g_gameProc, a_hwnd, a_msg, a_wparam, a_lparam);
			}
			return ::DefWindowProcW(a_hwnd, a_msg, a_wparam, a_lparam);
		}

		LRESULT CALLBACK WndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wparam, LPARAM a_lparam)
		{
			if (g_forwardingOriginal) {
				return ForwardToGame(a_hwnd, a_msg, a_wparam, a_lparam);
			}

			auto& runtime = Runtime::Get();

			// Reconcile the main-thread capture edge on the window thread.
			const bool wantHwCursor = runtime.IsInputCaptured();
			if (wantHwCursor != g_hwCursorActive) {
				g_hwCursorActive = wantHwCursor;
				if (wantHwCursor) {
					HardwareCursor::Activate(a_hwnd);
				} else {
					HardwareCursor::Deactivate();
				}
			}

			switch (a_msg) {
			case kRefreshInputStateMessage:
				// The capture/cursor edge was already reconciled above.
				return 0;
			case kRestoreGameFocusMessage:
				::SetFocus(a_hwnd);
				return 0;
			case WM_KEYDOWN:
			case WM_SYSKEYDOWN:
			{
				const auto vk = static_cast<std::uint32_t>(a_wparam);
				const bool repeat = (a_lparam & 0x40000000) != 0;
				// Route only the initial press so auto-repeat cannot retrigger toggles.
				const bool consume = repeat ? runtime.IsInputCaptured() :
					                              runtime.OnGameWindowKey(vk, MessageScanCode(vk, a_lparam), true);
				if (consume) {
					return 0;
				}
				break;
			}
			case WM_KEYUP:
			case WM_SYSKEYUP:
			{
				const auto vk = static_cast<std::uint32_t>(a_wparam);
				if (runtime.OnGameWindowKey(vk, MessageScanCode(vk, a_lparam), false)) {
					return 0;
				}
				break;
			}
			case WM_INPUTLANGCHANGE:
				// Flag layout changes for a main-thread keycap rebuild without consuming them.
				runtime.NotifyKeyboardLayoutChanged();
				break;
			case WM_CHAR:
				// Chromium receives native text and IME; swallow the game's duplicate stream while captured.
				if (runtime.IsInputCaptured()) return 0;
				break;
			case WM_UNICHAR:
				// Answer WM_UNICHAR probes and swallow duplicates only while captured.
				if (!runtime.IsInputCaptured()) {
					break;
				}
				if (a_wparam == UNICODE_NOCHAR) {
					return TRUE;  // yes, we accept WM_UNICHAR
				}
				return 0;
			case WM_DEADCHAR:
				// Block dead-key prefixes from the game while Chromium awaits the composed WM_CHAR.
				if (runtime.IsInputCaptured()) {
					return 0;
				}
				break;
			case WM_SETCURSOR:
				// Apply the page cursor and prevent engine reset if legacy WM_SETCURSOR arrives.
				if (g_hwCursorActive) {
					HardwareCursor::ApplyShape();
					return TRUE;
				}
				break;
			case WM_INPUT:
				if (runtime.IsInputCaptured()) {
					// Route to the overlay and use DefWindowProc only to release the raw-input buffer.
					RouteRawMouse(a_hwnd, a_lparam);
					return ::DefWindowProcW(a_hwnd, a_msg, a_wparam, a_lparam);
				}
				break;
			default:
				if (IsLegacyMouseMessage(a_msg) && runtime.IsInputCaptured()) {
					// Block legacy duplicates because WM_INPUT is authoritative.
					return 0;
				}
				break;
			}

			const auto current = reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(a_hwnd, GWLP_WNDPROC));
			if (detail::OriginalMovedAboveUs(
					reinterpret_cast<std::uintptr_t>(current),
					reinterpret_cast<std::uintptr_t>(&WndProc),
					reinterpret_cast<std::uintptr_t>(g_originalProc))) {
				if (!g_chainCycleLogged.exchange(true, std::memory_order_relaxed)) {
					REX::WARN("OverlayInputHook: the previously chained WndProc moved back above OSF UI; "
						"bypassing the circular link and forwarding to Starfield's class WndProc "
						"(compatibility path for BetterConsole and similar re-hooking overlays)");
				}
				return ForwardToGame(a_hwnd, a_msg, a_wparam, a_lparam);
			}

			g_forwardingOriginal = true;
			const auto result = ::CallWindowProcW(g_originalProc, a_hwnd, a_msg, a_wparam, a_lparam);
			g_forwardingOriginal = false;
			return result;
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
		// Call the stable class procedure so later subclass chains cannot recurse through ours.
		g_gameProc = reinterpret_cast<WNDPROC>(::GetClassLongPtrW(g_hwnd, GCLP_WNDPROC));
		if (!g_gameProc) {
			REX::WARN("OverlayInputHook: could not read the game window's class WndProc; "
				"recursive third-party hook recovery will fall back to DefWindowProc");
		}
		if (!g_originalProc) {
			REX::ERROR("OverlayInputHook: SetWindowLongPtr failed (Win32 error {})", ::GetLastError());
			return false;
		}

		REX::INFO("OverlayInputHook: subclassed game WndProc (hwnd 0x{:X}, class proc 0x{:X}); "
			"overlay can now capture input",
			reinterpret_cast<std::uintptr_t>(g_hwnd), reinterpret_cast<std::uintptr_t>(g_gameProc));
		return true;
	}

	void RequestStateRefresh()
	{
		if (g_hwnd) {
			::PostMessageW(g_hwnd, kRefreshInputStateMessage, 0, 0);
		}
	}

	void* GameWindowHandle()
	{
		return g_hwnd;
	}
}
