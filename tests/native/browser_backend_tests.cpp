#include "Render/BrowserBackendConfig.h"
#include "BrowserMessages.h"
#include "check.h"
#include <chrono>
#include <fstream>

int main()
{
    using namespace osfui::browser;
    const auto root = std::filesystem::temp_directory_path() /
        ("osfui-backend-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!std::filesystem::create_directory(root)) return 1;
    std::string error = "previous error";
    CHECK(OSFUI::ReadBrowserBackend(root, error) == Backend::WebView2);
    CHECK(error.empty());
    const auto write = [&](const char* text) { std::ofstream(root / "backend.json") << text; };
    for (const auto* invalid : {"{", "[]", "null", "{}", R"({"schemaVersion":2,"backend":"cef"})",
            R"({"schemaVersion":1,"backend":"unknown"})", R"({"schemaVersion":1,"backend":true})"}) {
        write(invalid);
        CHECK(!OSFUI::ReadBrowserBackend(root, error));
        CHECK(!error.empty());
    }
    write(R"({"schemaVersion":1,"backend":"webview2"})");
    CHECK(OSFUI::ReadBrowserBackend(root, error) == Backend::WebView2);
    CHECK(error.empty());
    write(R"({"schemaVersion":1,"backend":"cef"})");
    CHECK(OSFUI::ReadBrowserBackend(root, error) == Backend::Cef);
    CHECK(error.empty());

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
    std::filesystem::remove_all(root); // Exclusively created fixture directory.
    std::printf("browser_backend_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
