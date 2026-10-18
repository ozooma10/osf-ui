#include "BrowserMessages.h"
#include "check.h"

int main()
{
    using namespace osfui::browser;
    for (const auto backend : {Backend::WebView2, Backend::Cef}) {
        auto hello = msg::FromJson<msg::Hello>(msg::ToJson(msg::Hello{
            .protocolVersion = kBrowserHostProtocolVersion, .runtimeVersion = "unrelated version text",
            .backend = std::string(BackendName(backend)), .capabilities = kRequiredHostCapabilities }));
        CHECK(msg::IsCompatibleHello(hello, backend));
        CHECK(!msg::IsCompatibleHello(hello, backend == Backend::Cef ? Backend::WebView2 : Backend::Cef));
        hello.capabilities |= 1u << 30; // Future optional capabilities remain compatible.
        CHECK(msg::IsCompatibleHello(hello, backend));
        hello.capabilities &= ~kFrameAcknowledgments;
        CHECK(!msg::IsCompatibleHello(hello, backend));
        hello.capabilities = kRequiredHostCapabilities;
        --hello.protocolVersion;
        CHECK(!msg::IsCompatibleHello(hello, backend));
    }
    CHECK(!msg::IsCompatibleHello(msg::FromJson<msg::Hello>(nlohmann::json::object()), Backend::WebView2));
    std::printf("browser_backend_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
