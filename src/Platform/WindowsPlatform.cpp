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
