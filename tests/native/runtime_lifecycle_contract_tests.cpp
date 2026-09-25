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
    const auto capture = Read("../../src/Input/InputCaptureController.cpp");
    const auto plugin = Read("../../src/Core/Plugin.cpp");
    const auto frame = Read("../../src/Runtime/RuntimeFrame.cpp");
    const auto ids = Read("../../src/Core/Ids.h");

    const auto init = runtime.find("bool Runtime::Initialize()");
    const auto postLoad = runtime.find("void Runtime::OnPostLoad()");
    const auto prepare = runtime.find("bool Runtime::InitializeWebRuntime()");
    Check(init != std::string::npos && postLoad != std::string::npos && prepare != std::string::npos,
        "runtime entry points exist");
    if (failures) return failures;
    const auto initBody = runtime.substr(init, postLoad - init);
    Check(initBody.find("m_osfSettings.Initialize()") == std::string::npos,
		"plugin load does not acquire OSF Settings before peer plugins have loaded");
    Check(initBody.find("InitializeRenderer()") == std::string::npos &&
          initBody.find("InitializeCompositor()") == std::string::npos,
        "independent initialization does not construct the configured WebView runtime");
    Check(initBody.find("LoadStartupContent()") != std::string::npos &&
          initBody.find("InitializeBridge()") != std::string::npos,
        "manifests and bridge endpoints are available during plugin load");
    const auto postLoadBody = runtime.substr(postLoad, prepare - postLoad);
    Check(postLoadBody.find("m_osfSettings.Initialize()") != std::string::npos,
        "OSF Settings is acquired on SFSE kPostLoad");
    Check(postLoadBody.find("m_osfSettings.Initialize()") < postLoadBody.find("InitializeWebRuntime()") &&
          postLoadBody.find("InitializeWebRuntime()") < postLoadBody.find("InitializeStartupViews()"),
        "web runtime is prepared after Settings and before startup views are queued");
    Check(plugin.find("case SFSE::MessagingInterface::kPostLoad:") != std::string::npos,
        "dependency acquisition is dispatched at the Slim SDK lifecycle point");
    Check(frame.find("m_retainedState.Set") < frame.find("if (m_bridge)"),
        "owner state is retained before a lazy browser exists");
    const auto policy = runtime.substr(runtime.find("void Runtime::ApplyViewPresentationPolicy()"));
    Check(policy.find("ReconcileInputSuppression()") < policy.find("SetInputTargetView"),
        "hotkeys are blocked before the browser receives input focus");
    const auto prepareEnd = runtime.find("void Runtime::OnPostPostDataLoad()", prepare);
    const auto prepareBody = runtime.substr(prepare, prepareEnd - prepare);
    Check(prepareBody.find("InitializeRenderer()") != std::string::npos &&
          prepareBody.find("InitializeCompositor()") != std::string::npos,
        "renderer and compositor are constructed during web runtime preparation");
    Check(prepareBody.find("m_inputCapture.Initialize()") == std::string::npos,
        "engine input waits for data readiness, independently of renderer preparation");
    Check(plugin.find("case SFSE::MessagingInterface::kPostDataLoad:") == std::string::npos &&
          plugin.find("Runtime::Get().OnPostPostDataLoad()") != std::string::npos,
        "engine integration is consolidated after the data-load callbacks");
    const auto engine = runtime.find("void Runtime::InitializeEngineIntegration()");
    const auto engineBody = runtime.substr(engine, runtime.find("void Runtime::EnqueuePresentationRequest", engine) - engine);
    Check(engineBody.find("API::Papyrus::Install()") != std::string::npos &&
          engineBody.find("m_inputCapture.Initialize()") != std::string::npos &&
          frame.find("m_engineIntegrationPending.exchange(false") < frame.find("InitializeEngineIntegration()"),
        "the main-thread lifecycle stage installs session sinks and input before view requests");
    Check(capture.find("AcquireInputSuppression") != std::string::npos &&
          capture.find("ReleaseInputSuppression") != std::string::npos,
        "focused web menus hold an OSF Settings suppression lease");
    Check(input.find("VK_F10") == std::string::npos && runtime.find("VK_F10") == std::string::npos,
        "F10 has no OSF UI behavior");
    Check(ids.find("kStaleSettingsViewId") != std::string::npos &&
          ids.find("kStaleKeybindingsViewId") != std::string::npos,
        "removed built-in view IDs remain explicit rejection sentinels");

    std::cout << "runtime_lifecycle_contract_tests: " << failures << " failure(s)\n";
    return failures;
}
