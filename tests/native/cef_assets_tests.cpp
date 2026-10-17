#include "../../tools/cef_host/LocalAssets.h"
#include "check.h"
#include <chrono>
#include <fstream>
#include <stdexcept>

using namespace osfui::cefhost;

int main()
{
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
