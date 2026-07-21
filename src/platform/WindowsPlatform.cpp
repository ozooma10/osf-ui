#include "platform/WindowsPlatform.h"

// Keep <Windows.h> confined to this TU. NOGDI stops wingdi.h's ERROR macro from
// clobbering REX::ERROR; this file uses no REX logging, so it has no init-order
// requirements.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

#include <ShlObj.h>
#include <shellapi.h>  // ShellExecuteW (lean-and-mean excludes it)

namespace OSFUI::Platform
{
	bool OpenSystemBrowser(const wchar_t* a_url)
	{
		// ShellExecute contract: values > 32 are success; <= 32 are error codes.
		const auto rc = reinterpret_cast<std::intptr_t>(
			::ShellExecuteW(nullptr, L"open", a_url, nullptr, nullptr, SW_SHOWNORMAL));
		return rc > 32;
	}

	std::filesystem::path GetDocumentsPath()
	{
		PWSTR raw = nullptr;
		const auto hr = ::SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &raw);
		std::filesystem::path result;
		if (SUCCEEDED(hr) && raw) {
			result = raw;
		}
		if (raw) {
			::CoTaskMemFree(raw);
		}
		return result;
	}

	std::uint32_t DirectInputScanToVk(std::uint32_t a_scanCode)
	{
		// DIK codes are keyboard set-1 make codes; 0x80 marks extended keys
		// (the 0xE0 prefix byte), which VSC_TO_VK_EX takes in the high byte.
		const UINT composite = (a_scanCode & 0x80u) ? (0xE000u | (a_scanCode & 0x7Fu)) : a_scanCode;
		return ::MapVirtualKeyW(composite, MAPVK_VSC_TO_VK_EX);
	}

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
			// Truncated; grow and retry.
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

	bool IsReadableRange(const std::uintptr_t a_address, const std::size_t a_size)
	{
		if (a_address == 0 || a_size == 0) {
			return false;
		}

		std::uintptr_t cursor = a_address;
		const auto end = a_address + a_size;
		if (end < a_address) {
			// Range wraps the top of the address space (e.g. probing a garbage
			// value like 0xFFFF'FFFF'FFFF'FFFF): the walk below would be
			// vacuously true. Seen in the wild via UiPassSeam scanning -1 out
			// of a worker-stack blob.
			return false;
		}
		while (cursor < end) {
			MEMORY_BASIC_INFORMATION mbi{};
			if (::VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0) {
				return false;
			}
			if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 ||
				(mbi.Protect & 0xFF) == PAGE_NOACCESS) {
				return false;
			}
			const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
			if (regionEnd <= cursor) {
				return false;
			}
			cursor = regionEnd < end ? regionEnd : end;
		}
		return true;
	}

	bool SafeReadPointer(const std::uintptr_t a_address, std::uintptr_t& a_value)
	{
		if (!IsReadableRange(a_address, sizeof(std::uintptr_t))) {
			return false;
		}
		a_value = *reinterpret_cast<const std::uintptr_t*>(a_address);
		return true;
	}
}
