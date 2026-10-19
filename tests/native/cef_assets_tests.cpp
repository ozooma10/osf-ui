#include "../../tools/cef_host/LocalAssets.h"
#include "../../tools/cef_host/PendingMessages.h"
#include "../../tools/cef_host/AssetStream.h"
#include "../../tools/browser_shared/BrowserLocalUri.h"
#include "check.h"
#include <chrono>
#include <fstream>
#include <stdexcept>

using namespace osfui::cefhost;

int main()
{
    PendingMessages pending;
    for (int i = 0; i < 64; ++i) CHECK(pending.Push(std::to_string(i)));
    CHECK(!pending.Push("newest"));
    auto messages = pending.Take();
    CHECK(messages.size() == 64 && messages.front() == "1" && messages.back() == "newest");
    CHECK(pending.Take().empty());
    CHECK(pending.Push(std::string(PendingMessages::maxBytes, 'x')));
    CHECK(!pending.Push("replacement"));
    CHECK(!pending.Push(std::string(PendingMessages::maxBytes + 1, 'x')));
    messages = pending.Take();
    CHECK(messages.size() == 1 && messages.front() == "replacement");
    CHECK(pending.Push("after drain"));
    CHECK(SafeAssetPath(L"/acme/menu/index.html"));
    CHECK(SafeAssetPath(L"/shared/my image.png"));
    for (const auto* invalid : { L"", L"index.html", L"//server/file", L"/../outside.txt",
        L"/acme/./file", L"/acme/../../outside.txt", L"/acme\\..\\outside.txt", L"/C:/file",
        L"/acme/file:stream", L"/acme/dir./file", L"/acme/dir /file", L"/acme/%2e%2e/file", L"/acme/\nfile" })
        CHECK(!SafeAssetPath(invalid));
    CHECK(!SafeAssetPath(std::wstring_view(L"/acme/a\0b", 9)));

    const auto fixture = std::filesystem::temp_directory_path() /
        ("osfui-cef-assets-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!std::filesystem::create_directory(fixture)) throw std::runtime_error("fixture already exists");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } cleanup{ fixture }; // Exclusively created test directory, never a caller-supplied path.
    const auto root = fixture / "views";
    std::filesystem::create_directories(root / "acme" / "menu");
    std::ofstream(root / "acme" / "menu" / "index.html") << "fixture";
    std::ofstream(fixture / "outside.txt") << "outside";
    using osfui::browser::IsAllowedViewResourceUri;
    const auto host = osfui::browser::kViewHost;
    CHECK(IsAllowedViewResourceUri(L"data:text/plain,hello", host));
    CHECK(IsAllowedViewResourceUri(L"blob:https://osfui.example/id", host));
    CHECK(!IsAllowedViewResourceUri(L"blob:https://evil.example/id", host));
    CHECK(!IsAllowedViewResourceUri(L"blob:https://osfui.example.evil/id", host));
    CHECK(!IsAllowedViewResourceUri(L"file:///outside.txt", host));
    CHECK(!IsAllowedViewResourceUri(L"https://evil.example/", host));
    for (const auto range : { "bytes=1-3", "bytes=1-", "bytes=-3", "bytes=1-999" }) {
        AssetStream stream;
        CHECK(stream.Open(root / "acme/menu/index.html", false, range));
        CHECK(stream.response.status == 206);
        char buffer[32]{};
        const auto count = stream.Read(buffer, sizeof(buffer));
        CHECK(count == stream.response.length);
        CHECK(std::string(buffer, count) == std::string("fixture").substr(stream.response.offset, stream.response.length));
        CHECK(stream.Read(buffer, sizeof(buffer)) == 0);
    }
    for (const auto range : { "bytes=7-", "bytes=9-2", "bytes=-0" }) CHECK(SelectAssetRange(range, 7).status == 416);
    for (const auto range : { "", "bytes=no", "bytes=0-1,3-4", "bytes=999999999999999999999999-", "items=0-1" })
        CHECK(SelectAssetRange(range, 7).status == 200);
    CHECK(SelectAssetRange("bytes=0-", 0).status == 416);
    CHECK(SelectAssetRange("bytes=-2", 0).status == 416);
    AssetStream head;
    CHECK(head.Open(root / "acme/menu/index.html", true, "bytes=1-2"));
    CHECK(head.response.status == 200 && head.response.length == 7 && head.Remaining() == 0);
    AssetStream skipping;
    CHECK(skipping.Open(root / "acme/menu/index.html", false, ""));
    CHECK(skipping.Skip(3) == 3);
    char tail[16]{};
    CHECK(skipping.Read(tail, sizeof(tail)) == 4 && std::string(tail, 4) == "ture");
    CHECK(ResolveAsset(root, L"/acme/menu/index.html") == std::filesystem::canonical(root / "acme/menu/index.html"));
    CHECK(!ResolveAsset(root, L"/acme/menu"));
    CHECK(!ResolveAsset(root, L"/acme/menu/missing.html"));
    CHECK(!ResolveAsset(root, L"/../outside.txt"));
    CHECK(!ResolveAsset(fixture / "missing", L"/index.html"));
    std::error_code ec;
    std::filesystem::create_directory_symlink(fixture, root / "escape", ec);
    if (!ec) CHECK(!ResolveAsset(root, L"/escape/outside.txt"));
    else std::printf("SKIP symlink escape: platform denied symlink creation (%s)\n", ec.message().c_str());
    std::printf("cef_assets_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
