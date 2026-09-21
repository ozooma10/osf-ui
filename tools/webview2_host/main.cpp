#include "HostApp.h"

#include <shellapi.h>
#include <exception>
#include <string>
#include <vector>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	int argc = 0;
	LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
	if (!argv) {
		return 1;
	}

	osfui::wv2::HostOptions options;
	for (int i = 1; i < argc; ++i) {
		const std::wstring_view arg(argv[i]);
		if (arg.starts_with(L"--pipe=")) {
			options.pipeName = std::wstring(arg.substr(7));
		} else if (arg.starts_with(L"--game-pid=")) {
			options.gamePid = static_cast<std::uint32_t>(std::wcstoul(arg.substr(11).data(), nullptr, 10));
		} else if (arg.starts_with(L"--log=")) {
			options.logFile = std::filesystem::path(std::wstring(arg.substr(6)));
		} else if (arg.starts_with(L"--instance=")) {
			options.instance = std::wstring(arg.substr(11));
		}
	}
	::LocalFree(argv);

	if (options.pipeName.empty() || options.gamePid == 0) {
		return 1;
	}
	if (options.instance.size() > 32 || options.instance.find_first_not_of(
		L"abcdefghijklmnopqrstuvwxyz0123456789-") != std::wstring::npos) return 1;
	try {
		return osfui::wv2::RunHost(options);
	} catch (const std::exception&) {
		return 10;
	} catch (...) {
		return 10;
	}
}
