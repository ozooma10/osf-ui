#include "input/OverlayInputHook.h"

#include "input/HardwareCursor.h"
#include "runtime/Runtime.h"

// Keep <Windows.h> confined to this file. NOGDI stops wingdi's ERROR macro
// from clobbering REX::ERROR.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

namespace OSFUI::OverlayInputHook
{
	namespace
	{
		WNDPROC g_originalProc{ nullptr };
		HWND    g_hwnd{ nullptr };

		// A pending UTF-16 high surrogate from WM_CHAR, awaiting its low half to
		// form an astral-plane codepoint. Touched only on the window-message
		// thread (where WndProc runs), so a plain value is safe.
		std::uint16_t g_pendingHighSurrogate{ 0 };

		// Whether the hardware (OS) pointer is currently engaged. Capture flips
		// on the game main thread (ApplyMenuPolicy); this thread only observes
		// the edge, at the top of WndProc. Window-message thread only.
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

		// Route a WM_INPUT mouse packet into the overlay. This is the ONLY mouse
		// source: the game registers raw input in a way that suppresses the
		// legacy WM_MOUSE* stream (verified in-game 2026-07-01 — clicks routed
		// from legacy messages never arrived), so everything must come from the
		// raw packet.
		//
		// Position source depends on the cursor mode:
		//  - hardware cursor (a_hardwareCursor): the OS pointer is visible and
		//    authoritative — read its live position (GetCursorPos) and sync the
		//    runtime's view-space cursor to it, so buttons/hover land exactly
		//    where the user sees the pointer. Deltas are ignored.
		//  - fallback (config.hardwareCursor=false): the OS pointer stays
		//    hidden/clipped, so accumulate raw deltas into the virtual cursor.
		void RouteRawMouse(HWND a_hwnd, LPARAM a_lparam, bool a_hardwareCursor)
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

			if (a_hardwareCursor) {
				// The engine may re-hide/re-clip the pointer at any time; heal it
				// on the packet the user would notice it on.
				HardwareCursor::Reassert(a_hwnd);
				// Sync from the live OS pointer on EVERY packet (not just moves)
				// so even a click without a preceding move lands where the
				// pointer is.
				POINT pt{};
				RECT  client{};
				if (::GetCursorPos(&pt) && ::ScreenToClient(a_hwnd, &pt) &&
					::GetClientRect(a_hwnd, &client) && client.right > 0 && client.bottom > 0) {
					runtime.OnHostMouseAbsolute(pt.x, pt.y, client.right, client.bottom);
				}
			} else if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 && (mouse.lLastX != 0 || mouse.lLastY != 0)) {
				// Relative motion (absolute mode is for tablets/RDP — ignore it).
				runtime.OnHostMouseDelta(mouse.lLastX, mouse.lLastY);
			}

			const auto buttons = mouse.usButtonFlags;
			if (buttons & RI_MOUSE_LEFT_BUTTON_DOWN) {
				runtime.OnHostMouseButton(0, true);
			}
			if (buttons & RI_MOUSE_LEFT_BUTTON_UP) {
				runtime.OnHostMouseButton(0, false);
			}
			if (buttons & RI_MOUSE_RIGHT_BUTTON_DOWN) {
				runtime.OnHostMouseButton(1, true);
			}
			if (buttons & RI_MOUSE_RIGHT_BUTTON_UP) {
				runtime.OnHostMouseButton(1, false);
			}
			if (buttons & RI_MOUSE_MIDDLE_BUTTON_DOWN) {
				runtime.OnHostMouseButton(2, true);
			}
			if (buttons & RI_MOUSE_MIDDLE_BUTTON_UP) {
				runtime.OnHostMouseButton(2, false);
			}

			// Vertical wheel. usButtonData is a USHORT carrying a SIGNED
			// WHEEL_DELTA (120) multiple, so reinterpret it as short before
			// widening (positive = rotated forward/up).
			if (buttons & RI_MOUSE_WHEEL) {
				const auto wheelDelta = static_cast<short>(mouse.usButtonData);
				if (wheelDelta != 0) {
					runtime.OnHostMouseWheel(static_cast<int>(wheelDelta));
				}
			}
		}

		LRESULT CALLBACK WndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wparam, LPARAM a_lparam)
		{
			auto& runtime = Runtime::Get();

			// Reconcile the hardware pointer with the capture state on every
			// message: capture flips on the game main thread, so this is where
			// the open/close edge becomes visible to the window thread.
			const bool wantHwCursor = runtime.IsInputCaptured() && runtime.GetConfig().hardwareCursor;
			if (wantHwCursor != g_hwCursorActive) {
				g_hwCursorActive = wantHwCursor;
				if (wantHwCursor) {
					HardwareCursor::Activate(a_hwnd);
				} else {
					HardwareCursor::Deactivate();
				}
			}

			switch (a_msg) {
			case kRestoreGameFocusMessage:
				::SetFocus(a_hwnd);
				return 0;
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
			{
				// Text entry. wparam is one UTF-16 code unit (layout-, dead-key-,
				// and AltGr-resolved by Windows). Route real text into the overlay
				// and always block it from the game while captured; navigation /
				// editing keys (Enter, Tab, Backspace, Ctrl+letter -> control
				// chars) are handled via the VK/RawKeyDown path, so we drop them.
				if (!runtime.IsInputCaptured()) {
					break;
				}
				const auto unit = static_cast<std::uint16_t>(a_wparam);
				std::uint32_t codepoint = 0;
				if (unit >= 0xD800 && unit <= 0xDBFF) {
					g_pendingHighSurrogate = unit;  // wait for the low half
					return 0;
				}
				if (unit >= 0xDC00 && unit <= 0xDFFF) {
					if (g_pendingHighSurrogate == 0) {
						return 0;  // lone low surrogate; drop
					}
					codepoint = 0x10000u +
						((static_cast<std::uint32_t>(g_pendingHighSurrogate) - 0xD800u) << 10) +
						(static_cast<std::uint32_t>(unit) - 0xDC00u);
					g_pendingHighSurrogate = 0;
				} else {
					g_pendingHighSurrogate = 0;  // any BMP unit cancels a dangling high
					codepoint = unit;
				}
				if (codepoint >= 0x20 && codepoint != 0x7F) {
					runtime.OnHostChar(codepoint);
				}
				return 0;
			}
			case WM_UNICHAR:
				// UTF-32 char protocol. Answer the capability probe so senders may
				// use it, then route real text — only while captured, so the
				// game's own WM_UNICHAR handling is untouched when the overlay is
				// closed.
				if (!runtime.IsInputCaptured()) {
					break;
				}
				if (a_wparam == UNICODE_NOCHAR) {
					return TRUE;  // yes, we accept WM_UNICHAR
				}
				if (a_wparam >= 0x20 && a_wparam != 0x7F) {
					runtime.OnHostChar(static_cast<std::uint32_t>(a_wparam));
				}
				return 0;
			case WM_DEADCHAR:
				// A dead key (accent) — no finished character yet; the composed
				// result arrives as a later WM_CHAR. Just block it from the game
				// while captured.
				if (runtime.IsInputCaptured()) {
					return 0;
				}
				break;
			case WM_SETCURSOR:
				// Rarely (if ever) delivered — the game's raw-input registration
				// suppresses the legacy mouse stream — but if it does arrive,
				// apply the page's requested shape and keep the game's proc from
				// resetting/hiding the pointer. (Reassert covers the shape on
				// the WM_INPUT path.)
				if (g_hwCursorActive) {
					HardwareCursor::ApplyShape();
					return TRUE;
				}
				break;
			case WM_INPUT:
				if (runtime.IsInputCaptured()) {
					// The ONLY mouse source (see RouteRawMouse). Route into the
					// overlay, then consume so the game's camera/movement gets
					// nothing — WM_INPUT must still go to DefWindowProc to
					// release the raw input buffer (the game's proc is what
					// we skip).
					RouteRawMouse(a_hwnd, a_lparam, g_hwCursorActive);
					return ::DefWindowProcW(a_hwnd, a_msg, a_wparam, a_lparam);
				}
				break;
			default:
				if (IsLegacyMouseMessage(a_msg) && runtime.IsInputCaptured()) {
					// Everything already routes from WM_INPUT; block any legacy
					// duplicates from the game.
					return 0;
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
