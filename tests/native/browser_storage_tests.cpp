#include "Render/BrowserHostRuntime.h"
#include <Windows.h>
#include <cassert>
#include <fstream>
#include <iostream>

int main()
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / std::format("osfui-storage-test-{}-{}", GetCurrentProcessId(), GetTickCount64());
    assert(!fs::exists(root));
    const auto physical = OSFUI::ResolveBrowserStorageRoot(root);
    assert(fs::is_directory(root) && fs::is_directory(physical));
    assert(fs::equivalent(root, physical));
    // Resolve the returned path again: it must remain stable outside the alias.
    assert(OSFUI::ResolveBrowserStorageRoot(physical) == physical);
    const auto child = root / L"\u00e9\u65e5\u672c";
    const auto resolvedChild = OSFUI::ResolveBrowserStorageRoot(child);
    assert(fs::equivalent(child, resolvedChild));
    { std::ofstream out(resolvedChild / "proof.txt"); out << "same-data"; }
    std::ifstream input(child / "proof.txt");
    std::string text;
    input >> text;
    assert(text == "same-data");
    input.close();
    // A file cannot become a storage directory; preserve the existing failure path.
    assert(OSFUI::ResolveBrowserStorageRoot(child / "proof.txt") == child / "proof.txt");
    fs::remove_all(root);
    assert(!fs::exists(physical));
    std::cout << "browser_storage_tests: ok\n";
}
