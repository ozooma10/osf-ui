#include "runtime/DevViewFiles.h"

#include <cassert>
#include <fstream>
#include <iostream>

namespace
{
	void Write(const std::filesystem::path& a_path, std::string_view a_text)
	{
		std::filesystem::create_directories(a_path.parent_path());
		std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
		out << a_text;
		assert(out.good());
	}

	std::string Read(const std::filesystem::path& a_path)
	{
		std::ifstream in(a_path, std::ios::binary);
		return { std::istreambuf_iterator<char>(in), {} };
	}
}  // namespace

int main()
{
	namespace fs = std::filesystem;
	const auto root =
		fs::temp_directory_path() /
		("osfui-dev-view-files-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	const auto source = root / "source";
	const auto mirror = root / "mirror";

	// Enumeration/creation order cannot affect the fingerprint.
	const auto first = root / "first";
	const auto second = root / "second";
	Write(first / "a.js", "aa");
	Write(first / "nested" / "b.css", "bb");
	Write(second / "nested" / "b.css", "bb");
	Write(second / "a.js", "aa");
	const auto fixedTime = fs::file_time_type::clock::now() - std::chrono::seconds(10);
	for (const auto& relative : { fs::path("a.js"), fs::path("nested/b.css") }) {
		fs::last_write_time(first / relative, fixedTime);
		fs::last_write_time(second / relative, fixedTime);
	}
	assert(OSFUI::DevViewFiles::Fingerprint(first) == OSFUI::DevViewFiles::Fingerprint(second));

	// The root manifest is deliberately restart-only and cannot trigger reload.
	const auto beforeManifest = OSFUI::DevViewFiles::Fingerprint(first);
	Write(first / "manifest.json", R"({"id":"one"})");
	assert(OSFUI::DevViewFiles::Fingerprint(first) == beforeManifest);
	Write(first / "a.js", "cc");
	fs::last_write_time(first / "a.js", fixedTime + std::chrono::seconds(1));
	assert(OSFUI::DevViewFiles::Fingerprint(first) != beforeManifest);

	std::string error;
	Write(source / "index.html", "one");
	Write(source / "assets" / "old.js", "old");
	assert(OSFUI::DevViewFiles::SyncTree(source, mirror, error));
	assert(Read(mirror / "index.html") == "one");
	assert(Read(mirror / "assets" / "old.js") == "old");

	// Updates and additions copy; deletions disappear from the real-path mirror.
	Write(source / "index.html", "two");
	fs::remove(source / "assets" / "old.js");
	Write(source / "assets" / "new.js", "new");
	assert(OSFUI::DevViewFiles::SyncTree(source, mirror, error));
	assert(Read(mirror / "index.html") == "two");
	assert(!fs::exists(mirror / "assets" / "old.js"));
	assert(Read(mirror / "assets" / "new.js") == "new");

	// Renames may replace a directory with a file or vice versa.
	Write(source / "swap" / "child.txt", "child");
	assert(OSFUI::DevViewFiles::SyncTree(source, mirror, error));
	fs::remove_all(source / "swap");
	Write(source / "swap", "file");
	assert(OSFUI::DevViewFiles::SyncTree(source, mirror, error));
	assert(fs::is_regular_file(mirror / "swap"));
	assert(Read(mirror / "swap") == "file");

	fs::remove(source / "swap");
	Write(source / "swap" / "child.txt", "directory");
	assert(OSFUI::DevViewFiles::SyncTree(source, mirror, error));
	assert(fs::is_directory(mirror / "swap"));
	assert(Read(mirror / "swap" / "child.txt") == "directory");

	fs::remove_all(root);
	std::cout << "dev_view_files_tests: ok\n";
	return 0;
}
