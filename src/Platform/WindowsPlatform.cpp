#include "Platform/WindowsPlatform.h"

// Keep <Windows.h> here with NOGDI to avoid wingdi's ERROR macro.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

#include "REX/FModule.h"

#include "Win32Util.h"

namespace OSFUI::Platform
{
	std::uint32_t VkToDirectInputScan(std::uint32_t a_vk)
	{
		// Pin Pause, NumLock, and PrintScreen to DIK values rather than ambiguous API composites.
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

		// Convert VK_TO_VSC_EX prefixes to DIK's 0x80 extended-key convention.
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
		// UNCHANGED_REFCOUNT prevents diagnostics from retaining a plugin module.
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
