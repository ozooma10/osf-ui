#include "../../tools/webview2_host/MediaNavigation.h"
#include "check.h"
#include <iostream>

int main()
{
    using osfui::webview2::MediaNavigation;
    using osfui::webview2::VerifiedMediaDocumentMime;
    using nlohmann::json;
    constexpr auto url = "https://example.com/trailer.webm";
    MediaNavigation check;
    check.Start(10);
    CHECK(check.Begin(true, false, 9, 200, 10, url, 100));
    CHECK(!check.Ready(100)); // Completion may precede DOMContentLoaded.
    check.DomReady(9);
    CHECK(!check.Ready(100)); // Previous same-URL document is insufficient.
    check.DomReady(10);
    CHECK(check.Ready(100));
    CHECK(check.Matches(10, 2099));
    CHECK(!check.Matches(10, 2100));
    CHECK(!check.Matches(9, 100));
    CHECK(!check.Expired(2099));
    CHECK(check.Expired(2100));
    check.queryStarted = true;
    CHECK(!check.Ready(100)); // Exactly one query for this completion.
    check.Start(11);
    CHECK(!check.Matches(10, 100));
    CHECK(!check.pending);
    CHECK(check.domNavigationId == 0);
    CHECK(check.source.empty());
    check.DomReady(10);
    CHECK(check.domNavigationId == 0);
    CHECK(!check.Begin(true, false, 9, 200, 10, url, 100));
    CHECK(check.Begin(true, false, 9, 200, 11, url, 100));
    CHECK(!check.Ready(100)); // Reload of the same URL still requires a new DOM event.
    check.DomReady(11);
    CHECK(check.Ready(100));
    check.End();
    CHECK(!check.pending);
    CHECK(!check.Matches(11, 100));
    CHECK(!check.Ready(100));

    for (int status = 0; status != 19; ++status) {
        MediaNavigation candidate;
        candidate.Start(10);
        CHECK(candidate.Begin(true, false, status, 200, 10, url, 100) == (status == 9));
    }
    for (int http : {-1, 0, 199, 201, 204, 206, 299, 301, 400, 401, 403, 404, 500}) {
        MediaNavigation candidate;
        candidate.Start(10);
        CHECK(!candidate.Begin(true, false, 9, http, 10, url, 100));
    }
    check.Start(10);
    CHECK(!check.Begin(false, false, 9, 200, 10, url, 100));
    CHECK(!check.Begin(true, true, 9, 200, 10, url, 100));
    CHECK(!check.Begin(true, false, 9, 200, 0, url, 100));
    CHECK(!check.Begin(true, false, 9, 200, 10, "", 100));
    check.Start(0); // Process destruction/crash invalidates pending evidence.
    CHECK(!check.Begin(true, false, 9, 200, 10, url, 100));

    const json frame{{"id", "main"}, {"loaderId", "committed-loader"}, {"url", url}, {"mimeType", "video/webm"}};
    const auto wrap = [](const json& value) { return json{{"frameTree", {{"frame", value}}}}; };
    CHECK(VerifiedMediaDocumentMime(wrap(frame), url) == "video/webm");
    for (auto mime : {"audio/mpeg", "video/mp4", "audio/ogg"}) {
        auto changed = frame; changed["mimeType"] = mime;
        CHECK(VerifiedMediaDocumentMime(wrap(changed), url) == mime);
    }
    for (auto mime : {"text/html", "application/json", "application/octet-stream", "video/", "audio/",
        "videoevil/webm", "video/webm; charset=utf-8", "video/webm\n"}) {
        auto changed = frame; changed["mimeType"] = mime;
        CHECK(VerifiedMediaDocumentMime(wrap(changed), url).empty());
    }
    for (auto key : {"id", "loaderId", "url", "mimeType"}) {
        auto changed = frame; changed.erase(key);
        CHECK(VerifiedMediaDocumentMime(wrap(changed), url).empty());
        changed = frame; changed[key] = nullptr;
        CHECK(VerifiedMediaDocumentMime(wrap(changed), url).empty());
        changed[key] = "";
        CHECK(VerifiedMediaDocumentMime(wrap(changed), url).empty());
    }
    for (const json parent : {json("outer"), json(""), json(nullptr)}) {
        auto changed = frame; changed["parentId"] = parent;
        CHECK(VerifiedMediaDocumentMime(wrap(changed), url).empty());
    }
    for (const json unreachable : {json(url), json(0), json(nullptr)}) {
        auto changed = frame; changed["unreachableUrl"] = unreachable;
        CHECK(VerifiedMediaDocumentMime(wrap(changed), url).empty());
    }
    auto changed = frame; changed["unreachableUrl"] = "";
    CHECK(VerifiedMediaDocumentMime(wrap(changed), url) == "video/webm");
    changed = frame; changed["url"] = "chrome-error://chromewebdata/";
    CHECK(VerifiedMediaDocumentMime(wrap(changed), url).empty());
    CHECK(VerifiedMediaDocumentMime(wrap(frame), "https://example.com/another.webm").empty());
    changed = frame; changed["url"] = "file:///C:/trailer.webm";
    CHECK(VerifiedMediaDocumentMime(wrap(changed), "file:///C:/trailer.webm").empty());
    changed = frame; changed["urlFragment"] = "#t=10";
    CHECK(VerifiedMediaDocumentMime(wrap(changed), std::string(url) + "#t=10") == "video/webm");
    CHECK(VerifiedMediaDocumentMime(wrap(changed), url).empty());
    changed["urlFragment"] = nullptr;
    CHECK(VerifiedMediaDocumentMime(wrap(changed), url).empty());
    CHECK(VerifiedMediaDocumentMime(wrap(frame), std::string(url) + "#t=10").empty());
    changed = frame; changed["mimeType"] = "text/html";
    auto nested = wrap(changed); nested["frameTree"]["childFrames"] = json::array({{{"frame", frame}}});
    CHECK(VerifiedMediaDocumentMime(nested, url).empty()); // A video iframe cannot validate an HTML document.
    for (const json malformed : {json(nullptr), json::array(), json::object(), json{{"frameTree", nullptr}},
        json{{"frameTree", {{"frame", nullptr}}}}})
        CHECK(VerifiedMediaDocumentMime(malformed, url).empty());
    std::cout << "wv2_media_navigation_tests: " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures;
}
