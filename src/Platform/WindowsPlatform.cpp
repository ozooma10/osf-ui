#include "Platform/WindowsPlatform.h"

// Keep <Windows.h> confined to this TU. NOGDI stops wingdi.h's ERROR macro from
// clobbering REX::ERROR; this file uses no REX logging, so it has no init-order
// requirements.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

#include "REX/FModule.h"

#include "Win32Util.h"

namespace OSFUI::Platform
{
	using osfui::win32::ToUtf8;

	KeyLabelSource MakeKeyLabelSource(void* a_gameWindow)
	{
		const HWND hwnd = static_cast<HWND>(a_gameWindow);
		// Re-read per invocation: layouts are per-thread and switch at runtime.
		const auto layoutOf = [hwnd]() -> HKL {
			if (hwnd) {
				if (const DWORD tid = ::GetWindowThreadProcessId(hwnd, nullptr)) {
					return ::GetKeyboardLayout(tid);
				}
			}
			return ::GetKeyboardLayout(0);
		};

		KeyLabelSource source;
		source.glyph = [layoutOf](ScanCode a_scan) -> std::string {
			const HKL hkl = layoutOf();
			const UINT composite = (a_scan & 0x80u) ? (0xE000u | (a_scan & 0x7Fu)) : a_scan;
			const UINT vk = ::MapVirtualKeyExW(composite, MAPVK_VSC_TO_VK_EX, hkl);
			if (vk == 0) {
				return {};
			}
			BYTE    state[256]{};  // no modifiers, no CapsLock: the base keycap
			wchar_t buf[8]{};
			// Flag 0x4 (Win10 1607+, unconditionally available at our floor):
			// do not change keyboard state — probing a dead key ("^", "´")
			// must not poison the player's next composed character.
			int produced = ::ToUnicodeEx(vk, a_scan & 0x7Fu, state, buf, 8, 0x4, hkl);
			if (produced == -1) {
				produced = 1;  // dead key: buf[0] holds the spacing accent
			}
			if (produced <= 0) {
				return {};
			}
			std::wstring glyph(buf, buf + produced);
			// Control characters and whitespace are not keycaps (Tab, Enter
			// and friends label through the fixed table instead).
			if (glyph.size() == 1 && (glyph[0] < 0x20 || glyph[0] == 0x7F || glyph[0] == L' ')) {
				return {};
			}
			// Keycap form: uppercase a single alphabetic glyph ('ö' -> 'Ö';
			// CharUpperBuffW maps 'ß' to itself, which matches German keycaps).
			if (glyph.size() == 1) {
				::CharUpperBuffW(glyph.data(), 1);
			}
			return ToUtf8(glyph);
		};
		source.layoutName = [](ScanCode a_scan) -> std::string {
			// GetKeyNameTextW takes the lParam format: scan in bits 16-23,
			// extended in bit 24. Bit 25 ("don't care") stays clear so sided
			// modifiers keep distinct names. Uses the CALLING thread's layout
			// (no HKL parameter) — acceptable for a last-resort fallback.
			const LONG lparam = static_cast<LONG>(((a_scan & 0x7Fu) << 16) |
			                                      ((a_scan & 0x80u) ? (1u << 24) : 0u));
			wchar_t buf[64]{};
			const int n = ::GetKeyNameTextW(lparam, buf, 64);
			return n > 0 ? ToUtf8(std::wstring_view(buf, static_cast<std::size_t>(n))) : std::string{};
		};
		source.layoutTag = [layoutOf]() -> std::string {
			const HKL hkl = layoutOf();
			const auto langId = LOWORD(reinterpret_cast<ULONG_PTR>(hkl));
			wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
			if (::LCIDToLocaleName(MAKELCID(langId, SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0) > 0) {
				return ToUtf8(name);
			}
			return {};
		};
		return source;
	}

	std::uint32_t VkToDirectInputScan(std::uint32_t a_vk)
	{
		// Same three quirk keys ScanCode.h normalizes: Pause's EX mapping is an
		// 0xE1 composite whose base byte collides with RCtrl's, NumLock shares
		// Pause's raw make code, and PrintScreen is delivery-quirky — pin all
		// three to their DIK values instead of trusting the API's composite.
		switch (a_vk) {
		case VK_PAUSE:
			return 0xC5;
		case VK_NUMLOCK:
			return 0x45;
		case VK_SNAPSHOT:
			return 0xB7;
		default:
			break;
		}

		// VK_TO_VSC_EX returns 0xE0xx/0xE1xx composites for extended keys (the
		// 0xE0 prefix byte in the high byte); DIK codes are set-1 make codes
		// with 0x80 marking extended keys instead.
		const UINT composite = ::MapVirtualKeyW(a_vk, MAPVK_VK_TO_VSC_EX);
		if (composite == 0) {
			return 0;
		}
		const UINT prefix = composite >> 8;
		const UINT base = composite & 0xFFu;
		if (prefix == 0xE0u || prefix == 0xE1u) {
			return 0x80u | base;
		}
		return base;
	}

	std::string ModuleNameForAddress(const void* a_address)
	{
		if (!a_address) {
			return {};
		}
		HMODULE module = nullptr;
		// UNCHANGED_REFCOUNT: look the module up without taking a reference, so
		// this can never keep a plugin loaded in the process.
		if (!::GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(a_address),
				&module) ||
			!module) {
			return {};
		}
		const REX::FModule owner{ reinterpret_cast<REX::W32::HMODULE>(module) };
		return std::filesystem::path(owner.GetFileName()).filename().string();
	}

}
