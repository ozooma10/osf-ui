#include "../../tools/webview2_host/MediaNavigation.h"
#include "../../tools/webview2_host/DocumentNavigation.h"
#include "check.h"
#include <iostream>

int main()
{
    using osfui::webview2::MediaNavigation;
    using osfui::webview2::VerifiedMediaDocumentMime;
    using nlohmann::json;
    constexpr auto url = "https://example.com/trailer.webm";
    osfui::webview2::DocumentNavigation document;
    document.Start(1);
    document.Commit(1, false);
    CHECK(document.DomReady(1));
    document.Complete(true);
    document.Start(2); // Stop before commit retains the previous document and input.
    CHECK(document.Cancel(2, true));
    CHECK(!document.loading && document.usable && document.domReady);
    CHECK(!document.Cancel(2, true)); // Late/duplicate cancellation is inert.
    document.Start(3);
    document.Start(4); // Replaced navigation, including same-URL reloads.
    CHECK(!document.Cancel(3, true));
    CHECK(document.loading);
    CHECK(!document.DomReady(3));
    document.Commit(3, false);
    CHECK(!document.committed);
    document.Commit(4, false);
    CHECK(!document.usable && !document.domReady);
    document.Start(4); // A redirect callback with the same ID preserves state.
    CHECK(document.committed);
    CHECK(document.Cancel(4, true)); // Explicitly allow a stopped partial document.
    CHECK(!document.loading && document.usable && document.domReady);
    document.Start(5);
    document.Commit(5, true);
    CHECK(document.Cancel(5, true));
    CHECK(!document.loading && !document.usable && !document.domReady);
    CHECK(!document.DomReady(5)); // Error-page DOM must not admit input after Stop.
    document = {};
    document.Start(6);
    CHECK(document.Cancel(6, true)); // No proven document on the first pre-commit Stop.
    CHECK(!document.loading && !document.usable && !document.domReady);
    document.Start(7);
    document.Commit(7, false);
    CHECK(document.DomReady(7));
    CHECK(document.Cancel(7, false)); // Current source must still pass the origin guard.
    CHECK(!document.usable && !document.domReady);
    document = {}; // Renderer destruction invalidates every ID and document.
    CHECK(!document.Current(7));
    CHECK(!document.Cancel(0, true));

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
