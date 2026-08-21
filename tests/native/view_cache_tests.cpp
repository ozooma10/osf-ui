#include "Views/ViewCache.h"

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
}

int main()
{
	namespace fs = std::filesystem;
	const auto root = fs::temp_directory_path() /
		("osfui-view-cache-" +
			std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	const auto source = root / "source";
	const auto cache = root / "cache";
	Write(source / "shared" / "osfui.js", "shared-v1");
	Write(source / "osfui" / "settings" / "index.html", "settings-v1");
	std::string binary(64 * 1024, '\0');
	for (std::size_t i = 0; i < binary.size(); ++i) {
		binary[i] = static_cast<char>(i);
	}
	Write(source / "assets" / "exact-buffer.bin", binary);
	fs::create_directories(source / "empty");

	std::string error;
	const auto fingerprint = OSFUI::ViewCache::FingerprintTree(source, "runtime-v1", error);
	assert(fingerprint && error.empty());
	assert(fingerprint->files == 3);
	assert(fingerprint->bytes == 20 + binary.size());
	assert(OSFUI::ViewCache::GenerationName(fingerprint->value).starts_with("gen-"));

	const auto first = OSFUI::ViewCache::Prepare(
		source, cache, "runtime-v1", "first/process", error);
	assert(first && !first->reused && error.empty());
	assert(Read(first->generation / "shared" / "osfui.js") == "shared-v1");
	assert(Read(first->generation / "osfui" / "settings" / "index.html") ==
		"settings-v1");
	assert(Read(first->generation / "assets" / "exact-buffer.bin") == binary);
	assert(fs::is_directory(first->generation / "empty"));
	assert(fs::is_regular_file(first->generation / OSFUI::ViewCache::kCompleteMarker));
	assert(fs::is_regular_file(first->generation / OSFUI::ViewCache::kUseLock));

	// An identical source reuses the published immutable generation.
	const auto again = OSFUI::ViewCache::Prepare(
		source, cache, "runtime-v1", "second", error);
	assert(again && again->reused && again->generation == first->generation);

	// Same-size content changes publish a different generation even when an archive
	// or mod manager preserves the file timestamp.
	const auto preservedTime = fs::last_write_time(source / "shared" / "osfui.js");
	Write(source / "shared" / "osfui.js", "shared-v2");
	fs::last_write_time(source / "shared" / "osfui.js", preservedTime);
	const auto changed = OSFUI::ViewCache::Prepare(
		source, cache, "runtime-v1", "third", error);
	assert(changed && !changed->reused && changed->generation != first->generation);
	assert(Read(changed->generation / "shared" / "osfui.js") == "shared-v2");

	// Runtime/cache-format salt changes also invalidate an otherwise identical tree.
	const auto resalted = OSFUI::ViewCache::Prepare(
		source, cache, "runtime-v2", "fourth", error);
	assert(resalted && !resalted->reused && resalted->generation != changed->generation);

	// Incomplete target generations are never reused; Prepare replaces them only
	// after rebuilding a complete staging tree.
	const auto isolatedCache = root / "isolated-cache";
	const auto isolatedFingerprint =
		OSFUI::ViewCache::FingerprintTree(source, "runtime-v1", error);
	assert(isolatedFingerprint);
	const auto incomplete = isolatedCache /
		OSFUI::ViewCache::GenerationName(isolatedFingerprint->value);
	Write(incomplete / "stale.js", "partial");
	const auto repaired = OSFUI::ViewCache::Prepare(
		source, isolatedCache, "runtime-v1", "repair", error);
	assert(repaired && !repaired->reused);
	assert(!fs::exists(repaired->generation / "stale.js"));
	assert(Read(repaired->generation / "shared" / "osfui.js") == "shared-v2");

	// Scavenging removes abandoned staging and old generations, retains the
	// selected generation, and honors the caller's active-lease decision.
	const auto staging = cache / "staging-abandoned";
	const auto locked = cache / "gen-locked";
	Write(staging / "partial", "x");
	Write(locked / std::string(OSFUI::ViewCache::kUseLock), "");
	const auto scavenged = OSFUI::ViewCache::Scavenge(cache, resalted->generation,
		[&](const fs::path& a_path) { return a_path != locked; });
	assert(scavenged.removed == 3);  // first, changed, and abandoned staging
	assert(scavenged.retained == 2);  // current + simulated active generation
	assert(scavenged.failed == 0);
	assert(!fs::exists(first->generation));
	assert(!fs::exists(changed->generation));
	assert(!fs::exists(staging));
	assert(fs::exists(resalted->generation));
	assert(fs::exists(locked));

	assert(!OSFUI::ViewCache::Prepare(
		root / "missing", cache, "runtime-v1", "missing", error));
	assert(!error.empty());

	fs::remove_all(root);
	std::cout << "view_cache_tests: ok\n";
	return 0;
}
