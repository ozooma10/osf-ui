#include "HostApp.h"
#include "Wv2BrokerLaunch.h"
#include <format>

#include <shellapi.h>
#include <string>
#include <vector>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	int argc = 0;
	LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
	if (!argv) {
		return 1;
	}

	const std::wstring executable(argv[0]);
	bool broker = false;
	osfui::wv2::HostOptions options;
	for (int i = 1; i < argc; ++i) {
		const std::wstring_view arg(argv[i]);
		if (arg == L"--launch-broker") {
			broker = true;
		} else if (arg.starts_with(L"--pipe=")) {
			options.pipeName = std::wstring(arg.substr(7));
		} else if (arg.starts_with(L"--game-pid=")) {
			options.gamePid = static_cast<std::uint32_t>(std::wcstoul(arg.substr(11).data(), nullptr, 10));
		} else if (arg.starts_with(L"--log=")) {
			options.logFile = std::filesystem::path(std::wstring(arg.substr(6)));
		}
	}
	::LocalFree(argv);

	if (options.pipeName.empty() || options.gamePid == 0) {
		return 1;
	}
	if (broker) {
		const auto args = std::format(L"--pipe={} --game-pid={} --log=\"{}\"",
			options.pipeName, options.gamePid, options.logFile.native());
		const auto result = osfui::wv2::RunLaunchBroker(executable, args);
		return result.ok ? static_cast<int>(result.method) : 0;
	}
	return osfui::wv2::RunHost(options);
}
