#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
    int failures = 0;
    std::string Read(const char* path)
    {
        std::ifstream input(path, std::ios::binary);
        std::ostringstream out;
        out << input.rdbuf();
        return out.str();
    }
    void Check(bool condition, const char* message)
    {
        if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    }
}

int main()
{
    const auto runtime = Read("../../src/Runtime/Runtime.cpp");
    const auto input = Read("../../src/Input/RuntimeInput.cpp");
    const auto plugin = Read("../../src/Core/Plugin.cpp");
    const auto frame = Read("../../src/Runtime/RuntimeFrame.cpp");
    const auto ids = Read("../../src/Core/Ids.h");

    const auto init = runtime.find("bool Runtime::Initialize()");
    const auto postLoad = runtime.find("void Runtime::OnPostPostLoad()");
    const auto lazy = runtime.find("bool Runtime::EnsureWebRuntime()");
    Check(init != std::string::npos && postLoad != std::string::npos && lazy != std::string::npos,
        "runtime entry points exist");
    const auto initBody = runtime.substr(init, postLoad - init);
    Check(initBody.find("_osfSettings.Initialize()") == std::string::npos,
		"plugin load does not acquire OSF Settings before peer plugins have loaded");
    Check(initBody.find("InitializeRenderer()") == std::string::npos &&
          initBody.find("InitializeCompositor()") == std::string::npos,
        "lightweight initialization does not construct the WebView runtime");
    const auto postLoadBody = runtime.substr(postLoad, lazy - postLoad);
    Check(postLoadBody.find("_osfSettings.Initialize()") != std::string::npos,
        "OSF Settings is acquired on SFSE kPostPostLoad");
    Check(plugin.find("case SFSE::MessagingInterface::kPostPostLoad:") != std::string::npos,
        "dependency acquisition is dispatched at the Slim SDK lifecycle point");
    Check(frame.find("_retainedState.Set") < frame.find("if (_bridge)"),
        "owner state is retained before a lazy browser exists");
    const auto policy = runtime.substr(runtime.find("void Runtime::ApplyViewPresentationPolicy()"));
    Check(policy.find("ReconcileInputSuppression()") < policy.find("SetInputTargetView"),
        "hotkeys are blocked before the browser receives input focus");
    const auto lazyEnd = runtime.find("void Runtime::OnDataLoaded()", lazy);
    const auto lazyBody = runtime.substr(lazy, lazyEnd - lazy);
    Check(lazyBody.find("InitializeRenderer()") != std::string::npos &&
          lazyBody.find("InitializeCompositor()") != std::string::npos,
        "renderer and compositor construction are confined to the lazy path");
    Check(lazyBody.find("EnsureCaptureIntegration()") == std::string::npos,
        "web input is not installed merely by constructing the renderer");
    Check(input.find("AcquireInputSuppression") != std::string::npos &&
          input.find("ReleaseInputSuppression") != std::string::npos,
        "focused web menus hold an OSF Settings suppression lease");
    Check(input.find("VK_F10") == std::string::npos && runtime.find("VK_F10") == std::string::npos,
        "F10 has no OSF UI behavior");
    Check(ids.find("kStaleSettingsViewId") != std::string::npos &&
          ids.find("kStaleKeybindingsViewId") != std::string::npos,
        "removed built-in view IDs remain explicit rejection sentinels");

    std::cout << "runtime_lifecycle_contract_tests: " << failures << " failure(s)\n";
    return failures;
}
