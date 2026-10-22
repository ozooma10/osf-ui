#include "../../tools/webview2_host/CapturePerformance.h"
#include "Core/Json.h"
#include "check.h"

#include <memory>

namespace
{
    using nlohmann::json;
    using osfui::webview2::CapturePerformance;
    namespace Json = OSFUI::Json;

    std::wstring ToWide(const std::string& value) { return { value.begin(), value.end() }; }

    struct Page
    {
        unsigned replies{};
        void PostWebMessageAsString(const wchar_t*) { ++replies; }
    };

    struct View
    {
        std::string id{ "test/menu" };
        bool remoteContent{}, hidden{};
        std::unique_ptr<CapturePerformance> performance;
        std::unique_ptr<Page> webView{ std::make_unique<Page>() };
    };

    struct Host
    {
        bool devMode{}, windowActive{ true };
        unsigned width{ 1920 }, height{ 1080 };
        std::vector<std::unique_ptr<View>> views;
        struct Logger
        {
            std::vector<std::string> messages;
            void Info(const std::string& message) { messages.push_back(message); }
        } log;

        // Exercise the production handler and recording gates, including a page
        // sending the private protocol directly without using the F11 shortcut.
#include "../../tools/webview2_host/HostPerformance.inl"
    };

    std::string Request(const char* action, const char* session = "test")
    {
        return "__osfuiPerformance:" + json{ { "action", action }, { "session", session } }.dump();
    }
}

int main()
{
    Host host;
    auto& view = *host.views.emplace_back(std::make_unique<View>());
    CHECK(!host.HandlePerformanceMessage(view, "ordinary page message"));
    for (const auto* action : { "start", "sample", "pause", "stop" }) {
        CHECK(host.HandlePerformanceMessage(view, Request(action)));
        CHECK(!view.performance);
    }
    host.RecordPerformance(0); host.RecordPerformance(1); host.RecordPerformance(2);
    CHECK(view.webView->replies == 0 && host.log.messages.empty());

    host.devMode = true;
    view.remoteContent = true;
    CHECK(host.HandlePerformanceMessage(view, Request("start")));
    CHECK(!view.performance && view.webView->replies == 0);
    view.remoteContent = false;
    for (const auto* request : { "__osfuiPerformance:{", "__osfuiPerformance:[]",
            "__osfuiPerformance:{\"action\":\"start\"}" }) {
        CHECK(host.HandlePerformanceMessage(view, request));
        CHECK(!view.performance);
    }
    CHECK(host.HandlePerformanceMessage(view, Request("start")));
    CHECK(view.performance && view.performance->Active());
    CHECK(view.webView->replies == 1);
    host.RecordPerformance(0); host.RecordPerformance(1); host.RecordPerformance(2);
    auto snapshot = host.PerformanceSnapshot(view, CapturePerformance::Now());
    CHECK(snapshot["capture"]["totalFrames"] == 1);
    CHECK(snapshot["publication"]["totalFrames"] == 1);
    CHECK(snapshot["queueDeferrals"] == 1);
    CHECK(host.HandlePerformanceMessage(view, Request("stop", "stale")));
    CHECK(view.performance && host.log.messages.empty());

    view.hidden = true;
    host.RecordPerformance(0);
    CHECK(!view.performance->Active());
    CHECK(host.PerformanceSnapshot(view, CapturePerformance::Now())["capture"]["totalFrames"] == 1);
    view.hidden = false;
    CHECK(host.HandlePerformanceMessage(view, Request("start")));
    CHECK(view.performance->Active());
    host.windowActive = false;
    host.RecordPerformance(1);
    CHECK(!view.performance->Active());
    CHECK(host.HandlePerformanceMessage(view, Request("stop")));
    CHECK(!view.performance && host.log.messages.size() == 1);
    const auto replies = view.webView->replies;
    host.RecordPerformance(0); host.RecordPerformance(1); host.RecordPerformance(2);
    CHECK(!view.performance && view.webView->replies == replies && host.log.messages.size() == 1);
    std::printf("Performance developer gate: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
