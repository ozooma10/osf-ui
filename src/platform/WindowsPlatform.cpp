#include "platform/WindowsPlatform.h"

// Keep <Windows.h> confined to this translation unit. NOGDI prevents the ERROR
// macro (wingdi.h) from clobbering REX::ERROR elsewhere; we still avoid REX
// logging here entirely so this file has no ordering requirements.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

namespace SWUI::Platform
{
	std::filesystem::path GetThisModulePath()
	{
		HMODULE module = nullptr;
		if (!::GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&GetThisModulePath),
				&module)) {
			return {};
		}

		std::wstring buffer(MAX_PATH, L'\0');
		for (;;) {
			const auto len = ::GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
			if (len == 0) {
				return {};
			}
			if (len < buffer.size()) {
				buffer.resize(len);
				break;
			}
			// Path longer than the buffer; grow and retry.
			buffer.resize(buffer.size() * 2);
		}
		return std::filesystem::path(buffer);
	}

	bool LoadLibraryAbsolute(const std::filesystem::path& a_path, std::uint32_t& a_lastError)
	{
		a_lastError = 0;
		if (!a_path.is_absolute()) {
			a_lastError = ERROR_BAD_PATHNAME;
			return false;
		}
		if (::LoadLibraryExW(a_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) == nullptr) {
			a_lastError = ::GetLastError();
			return false;
		}
		return true;
	}
}
