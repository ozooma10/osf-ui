#include "Input/OverlayInputHook.h"

#include "Core/Log.h"
#include "Input/HardwareCursor.h"
#include "Input/BrowserKeyboard.h"
#include "Input/KeyOwnership.h"
#include "Win32Util.h"
#include "Runtime/Runtime.h"
#include "Wv2CdpInput.h"

// Keep <Windows.h> here with NOGDI to avoid wingdi's ERROR macro.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>
#include <imm.h>
#include "Wv2Protocol.h"

namespace OSFUI::OverlayInputHook
{
	namespace
	{
		std::atomic<WNDPROC> g_originalProc{ nullptr };
		std::atomic<WNDPROC> g_gameProc{ nullptr };
		HWND    g_hwnd{ nullptr };
		std::atomic_bool g_chainCycleLogged{ false };

		// Window-thread cursor state observes capture edges published by the main thread.
		bool g_hwCursorActive{ false };
		// Absolute raw-input devices report a normalized position rather than a
		// movement delta. Keep the last client position so relative-pointer owners
		// still receive motion on touchpads, virtual mice, and remapped devices.
		POINT g_lastAbsoluteClient{};
		bool  g_hasLastAbsoluteClient{ false };
		bool g_keyboardCaptured{ false };
		KeyOwnership g_keyOwnership;
		KeyOwnership g_rawKeyOwnership;
		bool g_imeComposing{ false };
		osfui::wv2::Utf16Input g_textInput;

		std::wstring ImeString(HIMC a_context, DWORD a_kind)
		{
			const LONG bytes = ::ImmGetCompositionStringW(a_context, a_kind, nullptr, 0);
			if (bytes <= 0 || bytes > 65536 || bytes % sizeof(wchar_t) != 0) return {};
			std::wstring text(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
			if (::ImmGetCompositionStringW(a_context, a_kind, text.data(), bytes) != bytes) return {};
			return text;
		}

		void ForwardCharacter(Runtime& a_runtime, char16_t a_character)
		{
			// Editing/navigation control characters are handled by rawKeyDown.
			if (a_character < 0x20 && a_character != u'\r') return;
			const auto text = g_textInput.Push(a_character);
			if (!text.empty()) {
				a_runtime.OnGameWindowText({ .text = osfui::win32::ToUtf8(std::wstring(text.begin(), text.end())) });
			}
		}

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
			const bool absoluteMove = (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0;
			if (!absoluteMove) {
				runtime.OnGameWindowMouseRelative(mouse.lLastX, mouse.lLastY);
				g_hasLastAbsoluteClient = false;
			}

			// Heal engine cursor changes on the next visible input packet.
			HardwareCursor::Reassert(a_hwnd);
			// Sync every packet so clicks without prior movement land correctly.
			POINT pt{};
			RECT  client{};
			if (::GetCursorPos(&pt) && ::ScreenToClient(a_hwnd, &pt) &&
				::GetClientRect(a_hwnd, &client) && client.right > 0 && client.bottom > 0) {
				if (absoluteMove) {
					if (g_hasLastAbsoluteClient) {
						runtime.OnGameWindowMouseRelative(
							pt.x - g_lastAbsoluteClient.x, pt.y - g_lastAbsoluteClient.y);
					}
					g_lastAbsoluteClient = pt;
					g_hasLastAbsoluteClient = true;
				}
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
			const auto gameProc = g_gameProc.load(std::memory_order_acquire);
			if (gameProc && gameProc != &WndProc) {
				return ::CallWindowProcW(gameProc, a_hwnd, a_msg, a_wparam, a_lparam);
			}
			return ::DefWindowProcW(a_hwnd, a_msg, a_wparam, a_lparam);
		}

		LRESULT CALLBACK WndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wparam, LPARAM a_lparam)
		{
			auto& runtime = Runtime::Get();

			// Reconcile the main-thread capture edge on the window thread.
			const bool wantHwCursor = runtime.IsInputCaptured() && ::GetForegroundWindow() == a_hwnd;
			const bool keyboardCaptured = runtime.IsInputCaptured();
			if (keyboardCaptured != g_keyboardCaptured) {
				g_keyboardCaptured = keyboardCaptured;
				g_textInput.Reset();
				const bool cancelComposition = !keyboardCaptured && g_imeComposing;
				g_imeComposing = false;
				if (cancelComposition) {
					if (const auto context = ::ImmGetContext(a_hwnd)) {
						::ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
						::ImmReleaseContext(a_hwnd, context);
					}
				}
			}
			if (wantHwCursor != g_hwCursorActive) {
				g_hwCursorActive = wantHwCursor;
				g_hasLastAbsoluteClient = false;
				if (wantHwCursor) {
					HardwareCursor::Activate(a_hwnd);
				} else {
					HardwareCursor::Deactivate();
				}
			}

			if (const auto refresh = osfui::wv2::RefreshInputStateMessage();
				refresh && a_msg == refresh) {
				// The capture/cursor edge was already reconciled above.
				return 0;
			}
			if (const auto restore = osfui::wv2::RestoreGameFocusMessage();
				restore && a_msg == restore) {
				// Never activate Starfield over another foreground application.
				if (::GetForegroundWindow() == a_hwnd) ::SetFocus(a_hwnd);
				return 0;
			}
			switch (a_msg) {
			case WM_SETFOCUS:
				runtime.OnGameWindowActivation(true);
				break;
			case WM_KILLFOCUS:
				g_keyOwnership.Reset();
				g_rawKeyOwnership.Reset();
				runtime.OnGameWindowActivation(false);
				g_textInput.Reset();
				g_imeComposing = false;
				HardwareCursor::Deactivate();
				g_hwCursorActive = false;
				break;
			case WM_KEYDOWN:
			case WM_SYSKEYDOWN:
			{
				const auto vk = static_cast<std::uint32_t>(a_wparam);
				const bool consume = runtime.OnGameWindowKeyboard(
					BrowserKeyboardEvent(vk, true, a_msg == WM_SYSKEYDOWN, a_lparam));
				if (g_keyOwnership.Consume(vk, true, consume)) {
					return 0;
				}
				break;
			}
			case WM_KEYUP:
			case WM_SYSKEYUP:
			{
				const auto vk = static_cast<std::uint32_t>(a_wparam);
				const bool consume = runtime.OnGameWindowKeyboard(
					BrowserKeyboardEvent(vk, false, a_msg == WM_SYSKEYUP, a_lparam));
				if (g_keyOwnership.Consume(vk, false, consume)) {
					return 0;
				}
				break;
			}
			case WM_CHAR:
				if (runtime.IsInputCaptured()) {
					if (!g_imeComposing) ForwardCharacter(runtime, static_cast<char16_t>(a_wparam));
					return 0;
				}
				break;
			case WM_SYSCHAR:
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
				if (a_wparam <= 0x10FFFF) {
					g_textInput.Reset();
					if (a_wparam >= 0x10000) {
						const auto scalar = static_cast<std::uint32_t>(a_wparam - 0x10000);
						ForwardCharacter(runtime, static_cast<char16_t>(0xD800 + (scalar >> 10)));
						ForwardCharacter(runtime, static_cast<char16_t>(0xDC00 + (scalar & 0x3FF)));
					} else if (a_wparam < 0xD800 || a_wparam > 0xDFFF) {
						ForwardCharacter(runtime, static_cast<char16_t>(a_wparam));
					}
				}
				return 0;
			case WM_IME_SETCONTEXT:
				if (runtime.IsInputCaptured()) {
					return ::DefWindowProcW(a_hwnd, a_msg, a_wparam, a_lparam & ~ISC_SHOWUICOMPOSITIONWINDOW);
				}
				break;
			case WM_IME_STARTCOMPOSITION:
				if (runtime.IsInputCaptured()) {
					g_imeComposing = true;
					g_textInput.Reset();
					return 0;
				}
				break;
			case WM_IME_COMPOSITION:
				if (runtime.IsInputCaptured()) {
					if (const auto context = ::ImmGetContext(a_hwnd)) {
						if (a_lparam & GCS_RESULTSTR) {
							runtime.OnGameWindowText({ .text = osfui::win32::ToUtf8(ImeString(context, GCS_RESULTSTR)), .kind = "commit" });
						}
						if (a_lparam & GCS_COMPSTR) {
							const auto text = ImeString(context, GCS_COMPSTR);
							const auto cursor = ::ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0);
							runtime.OnGameWindowText({ .text = osfui::win32::ToUtf8(text), .kind = "composition",
								.cursor = static_cast<std::uint32_t>(std::clamp<LONG>(cursor, 0, static_cast<LONG>(text.size()))) });
						}
						::ImmReleaseContext(a_hwnd, context);
					}
					return 0; // suppress DefWindowProc's duplicate WM_IME_CHAR commit
				}
				break;
			case WM_IME_ENDCOMPOSITION:
				if (runtime.IsInputCaptured()) {
					g_imeComposing = false;
					runtime.OnGameWindowText({ .text = "", .kind = "cancel" });
					return 0;
				}
				break;
			case WM_IME_CHAR:
				if (runtime.IsInputCaptured()) return 0;
				break;
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
			case WM_INPUT: {
				RAWINPUT raw{};
				UINT size = sizeof(raw);
				if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(a_lparam), RID_INPUT, &raw, &size,
					sizeof(RAWINPUTHEADER)) == size && raw.header.dwType == RIM_TYPEKEYBOARD) {
					if (g_rawKeyOwnership.Consume(raw.data.keyboard.VKey,
						(raw.data.keyboard.Flags & RI_KEY_BREAK) == 0, runtime.IsInputCaptured())) {
						return ::DefWindowProcW(a_hwnd, a_msg, a_wparam, a_lparam);
					}
					break; // forward releases whose down reached the game before capture
				}
				if (runtime.IsInputCaptured()) {
					// Route to the overlay and use DefWindowProc only to release the raw-input buffer.
					if (::GetForegroundWindow() == a_hwnd) RouteRawMouse(a_hwnd, a_lparam);
					return ::DefWindowProcW(a_hwnd, a_msg, a_wparam, a_lparam);
				}
				break;
			}
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
					reinterpret_cast<std::uintptr_t>(g_originalProc.load(std::memory_order_acquire)))) {
				if (!g_chainCycleLogged.exchange(true, std::memory_order_relaxed)) {
					REX::WARN("OverlayInputHook: the previously chained WndProc moved back above OSF UI; "
						"bypassing the circular link and forwarding to Starfield's class WndProc "
						"(compatibility path for BetterConsole and similar re-hooking overlays)");
				}
				return ForwardToGame(a_hwnd, a_msg, a_wparam, a_lparam);
			}

			// Forwarding can synchronously deliver different messages (for example,
			// WM_ACTIVATE -> WM_SETFOCUS); they must traverse our normal handler too.
			const auto original = g_originalProc.load(std::memory_order_acquire);
			return original ? ::CallWindowProcW(original, a_hwnd, a_msg, a_wparam, a_lparam) :
				ForwardToGame(a_hwnd, a_msg, a_wparam, a_lparam);
		}
	}

	bool Install()
	{
		if (g_originalProc.load(std::memory_order_acquire)) {
			return true;  // already installed (one-way)
		}

		g_hwnd = FindGameWindow();
		if (!g_hwnd) {
			REX::ERROR("OverlayInputHook: could not find the game window; input capture unavailable");
			return false;
		}

		// Keys already held when the hook is installed belong to the game.
		for (std::uint32_t vk = 1; vk < 256; ++vk) {
			if (::GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) {
				g_keyOwnership.Consume(vk, true, false);
				g_rawKeyOwnership.Consume(vk, true, false);
			}
		}
		// Call the stable class procedure so later subclass chains cannot recurse through ours.
		const auto gameProc = reinterpret_cast<WNDPROC>(::GetClassLongPtrW(g_hwnd, GCLP_WNDPROC));
		g_gameProc.store(gameProc, std::memory_order_release);
		if (!gameProc) {
			REX::WARN("OverlayInputHook: could not read the game window's class WndProc; "
				"recursive third-party hook recovery will fall back to DefWindowProc");
		}
		// Seed the forwarding target before publishing WndProc to the window
		// thread. SetWindowLongPtr returns the actual predecessor immediately after.
		g_originalProc.store(reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC)),
			std::memory_order_release);
		const auto original = reinterpret_cast<WNDPROC>(
			::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
		if (!original) {
			g_originalProc.store(nullptr, std::memory_order_release);
			REX::ERROR("OverlayInputHook: SetWindowLongPtr failed (Win32 error {})", ::GetLastError());
			return false;
		}
		g_originalProc.store(original, std::memory_order_release);

		REX::INFO("OverlayInputHook: subclassed game WndProc (hwnd 0x{:X}, class proc 0x{:X}); "
			"overlay can now capture input",
			reinterpret_cast<std::uintptr_t>(g_hwnd), reinterpret_cast<std::uintptr_t>(gameProc));
		return true;
	}

	void RequestStateRefresh()
	{
		if (g_hwnd) {
			if (const auto message = osfui::wv2::RefreshInputStateMessage()) {
				::PostMessageW(g_hwnd, message, 0, 0);
			}
		}
	}

	std::optional<ClientSize> GameWindowClientSize()
	{
		// Passive HUDs need the client size before any input hook is installed.
		if (!g_hwnd) g_hwnd = FindGameWindow();
		RECT client{};
		if (!g_hwnd || !::GetClientRect(g_hwnd, &client) || client.right <= client.left || client.bottom <= client.top) {
			return std::nullopt;
		}
		return ClientSize{
			.width = static_cast<std::uint32_t>(client.right - client.left),
			.height = static_cast<std::uint32_t>(client.bottom - client.top),
		};
	}
}
