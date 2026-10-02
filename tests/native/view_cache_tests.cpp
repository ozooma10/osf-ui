#include "Views/ViewCache.h"

#include <algorithm>
#include <cassert>
#include <chrono>
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

	const OSFUI::ViewCache::Prepared& Entry(const OSFUI::ViewCache::PreparedMods& a_mods, std::string_view a_name)
	{
		const auto found = std::ranges::find(a_mods.entries, a_name, &OSFUI::ViewCache::ModPrepared::name);
		assert(found != a_mods.entries.end());
		return found->cache;
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
	// Exercise a large tree: all concurrently copied files must finish intact before publication.
	constexpr std::size_t kExtraFiles = 96;
	std::uint64_t extraBytes = 0;
	for (std::size_t i = 0; i < kExtraFiles; ++i) {
		const auto content = std::string(i * 1024, static_cast<char>('a' + i % 26));
		Write(source / "many" / std::to_string(i % 8) / (std::to_string(i) + ".bin"), content);
		extraBytes += content.size();
	}
	fs::create_directories(source / "empty");

	std::string error;
	const auto fingerprint = OSFUI::ViewCache::FingerprintTree(source, "runtime-v1", error);
	assert(fingerprint && error.empty());
	assert(fingerprint->files == 3 + kExtraFiles);
	assert(fingerprint->bytes == 20 + binary.size() + extraBytes);
	assert(OSFUI::ViewCache::GenerationName(fingerprint->value).starts_with("gen-"));

	const auto first = OSFUI::ViewCache::Prepare(
		source, cache, "runtime-v1", "first/process", error);
	assert(first && !first->reused && error.empty());
	assert(first->generation.filename() == OSFUI::ViewCache::GenerationName(fingerprint->value));
	assert(Read(first->generation / "shared" / "osfui.js") == "shared-v1");
	assert(Read(first->generation / "osfui" / "settings" / "index.html") ==
		"settings-v1");
	assert(Read(first->generation / "assets" / "exact-buffer.bin") == binary);
	for (std::size_t i = 0; i < kExtraFiles; ++i) {
		assert(Read(first->generation / "many" / std::to_string(i % 8) / (std::to_string(i) + ".bin")) ==
			std::string(i * 1024, static_cast<char>('a' + i % 26)));
	}
	assert(fs::is_directory(first->generation / "empty"));
	assert(fs::is_regular_file(first->generation / OSFUI::ViewCache::kCompleteMarker));

	// An identical source reuses the published immutable generation.
	const auto again = OSFUI::ViewCache::Prepare(
		source, cache, "runtime-v1", "second", error);
	assert(again && again->reused && again->generation == first->generation);

	// Documented limitation: the fingerprint is metadata only, so a same-size edit whose
	// timestamp an archive or mod manager preserved still reuses the old generation.
	const auto preservedTime = fs::last_write_time(source / "shared" / "osfui.js");
	Write(source / "shared" / "osfui.js", "shared-v2");
	fs::last_write_time(source / "shared" / "osfui.js", preservedTime);
	const auto stale = OSFUI::ViewCache::Prepare(
		source, cache, "runtime-v1", "stale", error);
	assert(stale && stale->reused && stale->generation == first->generation);
	assert(Read(stale->generation / "shared" / "osfui.js") == "shared-v1");

	// A timestamp change alone publishes a different generation.
	fs::last_write_time(source / "shared" / "osfui.js", preservedTime + std::chrono::seconds(5));
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
	// selected generation, and leaves unrelated folders alone.
	const auto staging = cache / "staging-abandoned";
	const auto unrelated = cache / "unrelated";
	Write(staging / "partial", "x");
	Write(unrelated / "keep", "x");
	const auto scavenged = OSFUI::ViewCache::Scavenge(cache, resalted->generation);
	assert(scavenged.removed == 3);  // first, changed, and abandoned staging
	assert(scavenged.failed == 0);
	assert(!fs::exists(first->generation));
	assert(!fs::exists(changed->generation));
	assert(!fs::exists(staging));
	assert(fs::exists(resalted->generation));
	assert(fs::exists(unrelated));

	assert(!OSFUI::ViewCache::Prepare(
		root / "missing", cache, "runtime-v1", "missing", error));
	assert(!error.empty());


	const auto unicodeSource = root / "unicode-source";
	const auto unicodeName = fs::path(u8"麻雀/\U0001f3ae.js");
	Write(unicodeSource / unicodeName, "unicode asset");
	const auto unicode = OSFUI::ViewCache::Prepare(unicodeSource, cache, "unicode", "utf8", error);
	assert(unicode && Read(unicode->generation / unicodeName) == "unicode asset");
	const auto unicodeAgain = OSFUI::ViewCache::Prepare(unicodeSource, cache, "unicode", "utf8-again", error);
	assert(unicodeAgain && unicodeAgain->reused);

	// Independent mod caches keep the existing /mod/view and /shared browser paths.
	const auto modsSource = root / "mods-source";
	const auto modsCache = root / "mods-cache";
	Write(modsSource / "alpha" / "main" / "index.html", "alpha-v1");
	Write(modsSource / "beta" / "games" / "large.bin", binary);
	Write(modsSource / "shared" / "osfui.js", "kit-v1");
	Write(modsSource / "root.css", "root-v1");
	Write(modsSource / unicodeName, "unicode-mod");
	fs::create_directories(modsSource / "alpha" / "empty");
	const auto modsFirst = OSFUI::ViewCache::PrepareMods(modsSource, modsCache, "v1", "mods-first", error);
	assert(modsFirst && error.empty() && modsFirst->entries.size() == 5);
	assert(std::ranges::none_of(modsFirst->entries, [](const auto& mod) { return mod.cache.reused; }));
	assert(Read(modsFirst->root / "alpha" / "main" / "index.html") == "alpha-v1");
	assert(Read(modsFirst->root / "beta" / "games" / "large.bin") == binary);
	assert(Read(modsFirst->root / "shared" / "osfui.js") == "kit-v1");
	assert(Read(modsFirst->root / "root.css") == "root-v1");
	assert(Read(modsFirst->root / unicodeName) == "unicode-mod");
	assert(fs::is_directory(modsFirst->root / "alpha" / "empty"));
	const auto betaWritten = fs::last_write_time(modsFirst->root / "beta" / "games" / "large.bin");

	const auto modsAgain = OSFUI::ViewCache::PrepareMods(modsSource, modsCache, "v1", "mods-again", error);
	assert(modsAgain && modsAgain->root == modsFirst->root);
	assert(std::ranges::all_of(modsAgain->entries, [](const auto& mod) { return mod.cache.reused; }));

	// Updating the shared kit does not copy either mod's assets again.
	Write(modsSource / "shared" / "osfui.js", "kit-v2-longer");
	const auto sharedUpdate = OSFUI::ViewCache::PrepareMods(modsSource, modsCache, "v1", "shared-update", error);
	assert(sharedUpdate && !Entry(*sharedUpdate, "shared").reused);
	assert(Entry(*sharedUpdate, "alpha").reused && Entry(*sharedUpdate, "beta").reused);
	assert(fs::last_write_time(sharedUpdate->root / "beta" / "games" / "large.bin") == betaWritten);
	assert(Read(sharedUpdate->root / "shared" / "osfui.js") == "kit-v2-longer");

	// Updating/removing files within one mod replaces that complete subtree only.
	fs::remove(modsSource / "alpha" / "main" / "index.html");
	Write(modsSource / "alpha" / "main" / "new.html", "alpha-v2");
	const auto alphaUpdate = OSFUI::ViewCache::PrepareMods(modsSource, modsCache, "v1", "alpha-update", error);
	assert(alphaUpdate && !Entry(*alphaUpdate, "alpha").reused);
	assert(Entry(*alphaUpdate, "beta").reused && Entry(*alphaUpdate, "shared").reused);
	assert(!fs::exists(alphaUpdate->root / "alpha" / "main" / "index.html"));
	assert(Read(alphaUpdate->root / "alpha" / "main" / "new.html") == "alpha-v2");

	// Incomplete entries are repaired without touching other complete mods.
	Write(alphaUpdate->root / "alpha" / OSFUI::ViewCache::kCompleteMarker, "incomplete");
	const auto modRepair = OSFUI::ViewCache::PrepareMods(modsSource, modsCache, "v1", "mod-repair", error);
	assert(modRepair && !Entry(*modRepair, "alpha").reused && Entry(*modRepair, "beta").reused);
	assert(Read(modRepair->root / "alpha" / "main" / "new.html") == "alpha-v2");
	assert(!OSFUI::ViewCache::PrepareMods(root / "missing-mods", modsCache, "v1", "bad-source", error));
	assert(Read(modRepair->root / "alpha" / "main" / "new.html") == "alpha-v2");

	// Root assets can change type, and removed mods/root files cannot remain browser-visible.
	fs::remove(modsSource / "root.css");
	Write(modsSource / "root.css" / "nested.txt", "now-a-directory");
	const auto rootChanged = OSFUI::ViewCache::PrepareMods(modsSource, modsCache, "v1", "root-changed", error);
	assert(rootChanged && Read(rootChanged->root / "root.css" / "nested.txt") == "now-a-directory");
	fs::remove_all(modsSource / "root.css");
	fs::remove_all(modsSource / "alpha");
	const auto removedMod = OSFUI::ViewCache::PrepareMods(modsSource, modsCache, "v1", "removed-mod", error);
	assert(removedMod && removedMod->removed == 2 && Entry(*removedMod, "beta").reused);
	assert(!fs::exists(removedMod->root / "alpha") && !fs::exists(removedMod->root / "root.css"));
	#ifdef _WIN32
	fs::rename(modsSource / "beta", modsSource / "BETA");
	const auto caseRename = OSFUI::ViewCache::PrepareMods(modsSource, modsCache, "v1", "case-rename", error);
	assert(caseRename && Entry(*caseRename, "BETA").reused && caseRename->removed == 0);
	assert(Read(caseRename->root / "BETA" / "games" / "large.bin") == binary);
	fs::rename(modsSource / "BETA", modsSource / "beta");
	#endif

	// Retire the old monolithic layout and staged backups without deleting live mod caches.
	Write(modsCache / "gen-obsolete" / "old.bin", binary);
	const auto modScavenged = OSFUI::ViewCache::Scavenge(modsCache, removedMod->root);
	assert(modScavenged.failed == 0 && modScavenged.removed >= 4);
	assert(Read(removedMod->root / "beta" / "games" / "large.bin") == binary);
	assert(!fs::exists(modsCache / "gen-obsolete"));
	const auto modResalted = OSFUI::ViewCache::PrepareMods(modsSource, modsCache, "v2", "mod-resalt", error);
	assert(modResalted && !Entry(*modResalted, "beta").reused && !Entry(*modResalted, "shared").reused);

	fs::remove_all(root);
	std::cout << "view_cache_tests: ok\n";
	return 0;
}
