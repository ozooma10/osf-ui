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

#include "Win32Util.h"

namespace OSFUI::Platform
{
	namespace
	{
		// Windows only lets the process that currently owns the foreground raise a
		// window; anyone else is denied and their window merely flashes in the
		// taskbar. That is precisely our situation: the game owns the foreground,
		// so the browser/Explorer window the shell opens lands BEHIND a fullscreen
		// game and reads as "the button did nothing" — the bug this exists to fix.
		// Handing the right over first (ASFW_ANY, since the shell may satisfy the
		// request from an already-running explorer.exe whose pid we never learn)
		// lets the new window come to the front. Best-effort: if it fails the
		// window still opens, just behind, which is the old behaviour.
		void YieldForegroundToShell()
		{
			::AllowSetForegroundWindow(ASFW_ANY);
		}
	}

	bool OpenSystemBrowser(const wchar_t* a_url)
	{
		YieldForegroundToShell();
		// ShellExecute contract: values > 32 are success; <= 32 are error codes.
		const auto rc = reinterpret_cast<std::intptr_t>(
			::ShellExecuteW(nullptr, L"open", a_url, nullptr, nullptr, SW_SHOWNORMAL));
		return rc > 32;
	}

	bool OpenFolder(const std::filesystem::path& a_folder)
	{
		std::error_code ec;
		if (a_folder.empty() || !std::filesystem::is_directory(a_folder, ec)) {
			// Explorer would happily "open" a nonexistent path as a search; a
			// refusal the caller can report is more useful than a stray window.
			return false;
		}
		YieldForegroundToShell();
		const auto rc = reinterpret_cast<std::intptr_t>(
			::ShellExecuteW(nullptr, L"open", a_folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
		return rc > 32;
	}

	bool ConfirmBugReportUpload(std::string_view a_title)
	{
		auto title = osfui::win32::ToWide(a_title);
		if (title.empty()) title = L"(untitled report)";
		for (auto& character : title) {
			if (character < 0x20) character = L' ';
		}
		const auto message =
			L"Submit this diagnostic report?\n\n"
			L"Title: " + title +
			L"\n\nOSF UI.log and OSF UI.webview2-host.log will be redacted locally, "
			L"uploaded to OSF UI's private reporting service, and retained for up to "
			L"30 days. The report will be reviewed before any public GitHub issue is "
			L"created.\n\nChoose Yes only if you consent to this upload.";
		return ::MessageBoxW(nullptr, message.c_str(), L"OSF UI - Confirm diagnostic upload",
			MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND) == IDYES;
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

	std::string ModuleNameForAddress(const void* a_address)
	{
		if (!a_address) {
			return {};
		}
		HMODULE module = nullptr;
		// UNCHANGED_REFCOUNT: look the module up without taking a reference, so
		// this can never keep a plugin pinned in the process.
		if (!::GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(a_address),
				&module) ||
			!module) {
			return {};
		}
		std::wstring buffer(MAX_PATH, L'\0');
		const auto len = ::GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (len == 0 || len >= buffer.size()) {
			return {};
		}
		buffer.resize(len);
		return std::filesystem::path(buffer).filename().string();
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
