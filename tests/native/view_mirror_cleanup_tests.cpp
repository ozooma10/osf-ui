#include "runtime/ViewMirrorCleanup.h"

#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>

#ifdef _WIN32
#include <Windows.h>
#endif

int main()
{
	namespace fs = std::filesystem;
	using namespace OSFUI::ViewMirrorCleanup;
	const auto root = fs::temp_directory_path() /
		("osfui-mirror-cleanup-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	const auto alive = [](std::uint32_t a_pid) { return a_pid == 42; };
	assert(Scavenge(root, false, alive).failed == 0);

	const auto write = [&](std::string_view a_name) {
		const auto dir = root / a_name;
		fs::create_directories(dir);
		std::ofstream file(dir / "keep.txt");
		file << "fixture";
		assert(file.good());
	};
	for (const auto name : { "views-mirror", "views-mirror-41", "views-mirror-42",
		"views-mirror-43", "WebView2", "bin", "settings", "views-cache", "views-dev-41",
		"views-mirror-backup", "views-mirror-", "views-mirror-0", "views-mirror-4294967296",
		"views-mirror-41-research", "views-mirror-41x", "views-mirror-+41" }) {
		write(name);
	}
	std::ofstream(root / "views-mirror-44") << "not a directory";

	const auto first = Scavenge(root, true, alive);
	assert(first.removed == 2 && first.failed == 0);
	assert(!fs::exists(root / "views-mirror-41"));
	assert(!fs::exists(root / "views-mirror-43"));
	for (const auto name : { "views-mirror", "views-mirror-42", "WebView2", "bin", "settings",
		"views-cache", "views-dev-41", "views-mirror-backup", "views-mirror-",
		"views-mirror-0", "views-mirror-4294967296", "views-mirror-41-research",
		"views-mirror-41x", "views-mirror-+41" }) {
		assert(fs::exists(root / name / "keep.txt"));
	}
	assert(fs::is_regular_file(root / "views-mirror-44"));
	const auto second = Scavenge(root, false, alive);
	assert(second.removed == 1 && second.failed == 0);
	assert(!fs::exists(root / "views-mirror"));
	assert(Scavenge(root, false, alive).removed == 0);

#ifdef _WIN32
	// A browser-held file can block deletion: keep it and retry on a later launch.
	write("views-mirror-45");
	const auto lockedPath = root / "views-mirror-45" / "keep.txt";
	const HANDLE locked = ::CreateFileW(lockedPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	assert(locked != INVALID_HANDLE_VALUE);
	const auto blocked = Scavenge(root, false, alive);
	assert(blocked.removed == 0 && blocked.failed == 1);
	assert(fs::exists(lockedPath));
	::CloseHandle(locked);
	const auto retried = Scavenge(root, false, alive);
	assert(retried.removed == 1 && retried.failed == 0);
#endif

	fs::remove_all(root);
	std::cout << "view_mirror_cleanup_tests: ok\n";
}
