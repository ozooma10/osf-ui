#pragma once

// Win32 helpers. All direct Win32 usage lives in this file pair.

namespace OSFUI::Platform
{
	// Full path of the DLL this code is compiled into (not the host EXE).
	// Returns an empty path on failure.
	[[nodiscard]] std::filesystem::path GetThisModulePath();

	// The user's Documents folder (FOLDERID_Documents — follows OneDrive
	// redirection). Empty on failure. Base for persisted, writable data (e.g.
	// settings values), which cannot live under the read-only,
	// MO2/Program-Files-mapped plugin data folder.
	[[nodiscard]] std::filesystem::path GetDocumentsPath();

	// DirectInput (DIK) scan code -> Windows VK code on the current keyboard
	// layout (MapVirtualKey VSC_TO_VK_EX; DIK codes are set-1 make codes with
	// 0x80 marking extended keys). 0 when untranslatable. Feeds VanillaKeys'
	// controlmap overlays (mcm-design.md §9 "vanilla hotkeys").
	[[nodiscard]] std::uint32_t DirectInputScanToVk(std::uint32_t a_scanCode);

	// Opens a URL in the user's default web browser (ShellExecuteW "open").
	// False when the shell refused. The caller owns deciding WHAT may open —
	// pass compile-time constants only, never web-supplied strings
	// (docs/security-model.md: no URL-steering from page content).
	bool OpenSystemBrowser(const wchar_t* a_url);

	// Opens a FOLDER in the shell's file browser. Refuses anything that is not
	// an existing directory, so a caller cannot turn this into "run whatever
	// this path points at". Same rule as OpenSystemBrowser: the caller decides
	// WHAT may open, and web content never supplies the target
	// (docs/security-model.md).
	bool OpenFolder(const std::filesystem::path& a_folder);

	struct HttpResponse
	{
		bool          transportOk{ false };
		std::uint32_t status{ 0 };
		std::string   body;
		std::string   error;
	};

	// Synchronous HTTPS POST used only from the reporting worker thread. TLS
	// verification remains at WinHTTP defaults; redirects and non-HTTPS URLs
	// are refused. Response bodies are capped.
	[[nodiscard]] HttpResponse PostJson(std::string_view a_url, std::string_view a_body);

	// True when [a_address, a_address + a_size) is committed, non-guard,
	// readable memory (VirtualQuery walk). For probing engine pointers.
	[[nodiscard]] bool IsReadableRange(std::uintptr_t a_address, std::size_t a_size);

	// Reads one pointer-sized value if the location is readable.
	[[nodiscard]] bool SafeReadPointer(std::uintptr_t a_address, std::uintptr_t& a_value);
}
