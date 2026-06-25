#include "platform/WindowsPlatform.h"

// Keep <Windows.h> confined to this translation unit. NOGDI prevents the ERROR
// macro (wingdi.h) from clobbering REX::ERROR elsewhere; we still avoid REX
// logging here entirely so this file has no ordering requirements.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

#include <ShlObj.h>

#include <cstring>

namespace PrismaSF::Platform
{
	namespace
	{
		// The clipboard is a single global resource other processes also grab;
		// a short bounded retry rides out transient contention without blocking
		// the worker for long.
		bool OpenClipboardWithRetry()
		{
			for (int attempt = 0; attempt < 5; ++attempt) {
				if (::OpenClipboard(nullptr)) {
					return true;
				}
				::Sleep(1);
			}
			return false;
		}
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

	bool IsReadableRange(const std::uintptr_t a_address, const std::size_t a_size)
	{
		if (a_address == 0 || a_size == 0) {
			return false;
		}

		std::uintptr_t cursor = a_address;
		const auto end = a_address + a_size;
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

	std::wstring GetClipboardText()
	{
		if (!::IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboardWithRetry()) {
			return {};
		}
		std::wstring result;
		if (const HANDLE data = ::GetClipboardData(CF_UNICODETEXT)) {
			if (const auto* text = static_cast<const wchar_t*>(::GlobalLock(data))) {
				// CF_UNICODETEXT is NUL-terminated, but GlobalSize is the upper
				// bound — scan to the NUL within it so a malformed block can't
				// over-read.
				const std::size_t maxChars = ::GlobalSize(data) / sizeof(wchar_t);
				std::size_t       len = 0;
				while (len < maxChars && text[len] != L'\0') {
					++len;
				}
				result.assign(text, len);
				::GlobalUnlock(data);
			}
		}
		::CloseClipboard();
		return result;
	}

	bool SetClipboardText(const std::wstring& a_text)
	{
		if (!OpenClipboardWithRetry()) {
			return false;
		}
		bool ok = false;
		if (::EmptyClipboard()) {
			const std::size_t bytes = (a_text.size() + 1) * sizeof(wchar_t);  // incl NUL
			if (const HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
				if (auto* dst = static_cast<wchar_t*>(::GlobalLock(mem))) {
					std::memcpy(dst, a_text.c_str(), bytes);
					::GlobalUnlock(mem);
					// On success the system takes ownership of `mem`; on failure
					// we must free it ourselves.
					if (::SetClipboardData(CF_UNICODETEXT, mem)) {
						ok = true;
					} else {
						::GlobalFree(mem);
					}
				} else {
					::GlobalFree(mem);
				}
			}
		}
		::CloseClipboard();
		return ok;
	}

	bool ClearClipboard()
	{
		if (!OpenClipboardWithRetry()) {
			return false;
		}
		const bool ok = ::EmptyClipboard() != 0;
		::CloseClipboard();
		return ok;
	}
}
