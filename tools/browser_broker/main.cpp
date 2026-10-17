#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#include <filesystem>
#include <format>
#include "BrowserBrokerLaunch.h"

// This small process may inherit MO2's injection. Chromium's sandbox bootstrap
// must only start after the broker has crossed into the clean desktop process.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 0;
    std::wstring host, pipe, pid, log;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg.starts_with(L"--host=")) host = arg.substr(7);
        else if (arg.starts_with(L"--pipe=")) pipe = arg.substr(7);
        else if (arg.starts_with(L"--game-pid=")) pid = arg.substr(11);
        else if (arg.starts_with(L"--log=")) log = arg.substr(6);
    }
    LocalFree(argv);
    if (!std::filesystem::path(host).is_absolute() || pipe.empty() || pid.empty() || log.empty()) return 0;
    const auto args = std::format(L"--pipe={} --game-pid={} --log=\"{}\"", pipe, pid, log);
    const auto result = osfui::browser::RunLaunchBroker(host, args);
    return result.ok ? static_cast<int>(result.method) : 0;
}
