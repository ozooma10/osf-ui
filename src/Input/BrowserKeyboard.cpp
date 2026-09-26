#include "Input/BrowserKeyboard.h"
#include "Win32Util.h"
#include "Wv2Messages.h"

namespace OSFUI
{
	namespace
	{
		std::string NamedKey(std::uint32_t vk)
		{
			switch (vk) {
			case VK_BACK: return "Backspace";
			case VK_TAB: return "Tab";
			case VK_RETURN: return "Enter";
			case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return "Shift";
			case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return "Control";
			case VK_MENU: case VK_LMENU: case VK_RMENU: return "Alt";
			case VK_PAUSE: return "Pause";
			case VK_CAPITAL: return "CapsLock";
			case VK_ESCAPE: return "Escape";
			case VK_PRIOR: return "PageUp";
			case VK_NEXT: return "PageDown";
			case VK_END: return "End";
			case VK_HOME: return "Home";
			case VK_LEFT: return "ArrowLeft";
			case VK_UP: return "ArrowUp";
			case VK_RIGHT: return "ArrowRight";
			case VK_DOWN: return "ArrowDown";
			case VK_INSERT: return "Insert";
			case VK_DELETE: return "Delete";
			case VK_SNAPSHOT: return "PrintScreen";
			case VK_LWIN: case VK_RWIN: return "Meta";
			case VK_APPS: return "ContextMenu";
			case VK_NUMLOCK: return "NumLock";
			case VK_SCROLL: return "ScrollLock";
			default:
				if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
				return {};
			}
		}

		std::string PhysicalCode(std::uint32_t vk, std::uint32_t scan, bool extended, const std::string& named)
		{
			if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) return scan == 0x36 ? "ShiftRight" : "ShiftLeft";
			if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) return extended ? "ControlRight" : "ControlLeft";
			if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) return extended ? "AltRight" : "AltLeft";
			if (vk == VK_LWIN || vk == VK_RWIN) return vk == VK_LWIN ? "MetaLeft" : "MetaRight";
			if (vk == VK_RETURN && extended) return "NumpadEnter";
			if (!extended && scan >= 0x47 && scan <= 0x53) {
				constexpr const char* codes[]{ "Numpad7", "Numpad8", "Numpad9", "NumpadSubtract",
					"Numpad4", "Numpad5", "Numpad6", "NumpadAdd", "Numpad1", "Numpad2", "Numpad3", "Numpad0", "NumpadDecimal" };
				return codes[scan - 0x47];
			}
			if (vk == VK_MULTIPLY) return "NumpadMultiply";
			if (vk == VK_DIVIDE) return "NumpadDivide";
			if (!named.empty()) return named;
			// DOM code is the physical US-layout position; DOM key is layout-dependent.
			if (scan >= 0x10 && scan <= 0x19) return std::string("Key") + "QWERTYUIOP"[scan - 0x10];
			if (scan >= 0x1E && scan <= 0x26) return std::string("Key") + "ASDFGHJKL"[scan - 0x1E];
			if (scan >= 0x2C && scan <= 0x32) return std::string("Key") + "ZXCVBNM"[scan - 0x2C];
			if (scan >= 2 && scan <= 11) return std::string("Digit") + "1234567890"[scan - 2];
			switch (scan) {
			case 0x0C: return "Minus";
			case 0x0D: return "Equal";
			case 0x1A: return "BracketLeft";
			case 0x1B: return "BracketRight";
			case 0x27: return "Semicolon";
			case 0x28: return "Quote";
			case 0x29: return "Backquote";
			case 0x2B: return "Backslash";
			case 0x33: return "Comma";
			case 0x34: return "Period";
			case 0x35: return "Slash";
			case 0x39: return "Space";
			case 0x56: return "IntlBackslash";
			default: return {};
			}
		}
	}

	osfui::wv2::msg::Keyboard BrowserKeyboardEvent(std::uint32_t a_vk, bool a_down,
		bool a_system, std::intptr_t a_lparam)
	{
		const auto scan = static_cast<UINT>((a_lparam >> 16) & 0xFF);
		const bool extended = (a_lparam & 0x01000000) != 0;
		const auto held = [](int vk) { return (::GetKeyState(vk) & 0x8000) != 0; };
		osfui::wv2::msg::Keyboard event;
		event.vk = a_vk;
		event.down = a_down;
		event.repeat = a_down && (a_lparam & 0x40000000) != 0;
		event.system = a_system;
		event.modifiers = (held(VK_MENU) ? 1u : 0u) | (held(VK_CONTROL) ? 2u : 0u) |
			((held(VK_LWIN) || held(VK_RWIN)) ? 4u : 0u) | (held(VK_SHIFT) ? 8u : 0u);
		event.key = NamedKey(a_vk);
		event.code = PhysicalCode(a_vk, scan, extended, event.key);
		event.keypad = event.code.starts_with("Numpad");
		if (event.code.ends_with("Left") && (event.key == "Shift" || event.key == "Control" || event.key == "Alt" || event.key == "Meta")) event.location = 1;
		if (event.code.ends_with("Right") && (event.key == "Shift" || event.key == "Control" || event.key == "Alt" || event.key == "Meta")) event.location = 2;
		// CDP uses isKeypad separately and only accepts left/right in location.
		if (event.key.empty()) {
			BYTE state[256]{};
			::GetKeyboardState(state);
			// Ctrl shortcuts still have printable DOM key names. Preserve AltGr.
			if (!held(VK_MENU)) state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_RCONTROL] = 0;
			wchar_t text[8]{};
			// Flag 4 avoids changing Windows' dead-key composition state.
			const int count = ::ToUnicodeEx(a_vk, scan, state, text, 8, 4, ::GetKeyboardLayout(0));
			if (count < 0) event.key = "Dead";
			else if (count > 0) event.key = osfui::win32::ToUtf8(std::wstring_view(text, static_cast<std::size_t>(count)));
		}
		return event;
	}

	osfui::wv2::msg::Keyboard BrowserNavigationKeyEvent(std::uint32_t a_vk, bool a_down)
	{
		osfui::wv2::msg::Keyboard event;
		event.vk = a_vk;
		event.down = a_down;
		event.key = NamedKey(a_vk);
		event.code = event.key;
		return event;
	}
}
