#include "Views/ViewCache.h"
#include "Core/Utf8Path.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <span>
#include <thread>
#include <vector>

namespace OSFUI::ViewCache
{
	namespace
	{
		constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
		constexpr std::uint64_t kFnvPrime = 1099511628211ull;
		// 2: generations are identified by file metadata instead of file content.
		constexpr std::uint64_t kCacheFormat = 2;

		struct FileStamp
		{
			std::filesystem::path source;
			std::filesystem::path relativePath;
			std::string           relative;
			std::uintmax_t        size{ 0 };
			std::int64_t          modified{ 0 };  // file_time_type ticks
		};

		struct TreeSnapshot
		{
			std::vector<std::filesystem::path> directories;
			std::vector<FileStamp>             files;
		};

		std::uint64_t Mix(std::uint64_t a_hash, std::uint64_t a_value)
		{
			return (a_hash ^ a_value) * kFnvPrime;
		}

		void MixText(std::uint64_t& a_hash, std::string_view a_text)
		{
			for (const unsigned char byte : a_text) {
				a_hash = Mix(a_hash, byte);
			}
			a_hash = Mix(a_hash, 0);
		}

		bool Fail(std::string& a_error, const std::filesystem::path& a_path, const std::error_code& a_ec)
		{
			a_error = Utf8Path(a_path.filename()) + ": " + a_ec.message();
			return false;
		}

		bool SnapshotTree(const std::filesystem::path& a_source,
			TreeSnapshot& a_snapshot, std::string& a_error, const SkipFile& a_skip = {})
		{
			std::error_code ec;
			if (!std::filesystem::is_directory(a_source, ec) || ec) {
				a_error = ec ? ec.message() : "source is not a directory";
				return false;
			}

			a_snapshot = {};
			for (std::filesystem::recursive_directory_iterator it(a_source, ec), end;
				 !ec && it != end; it.increment(ec)) {
				const auto relative = it->path().lexically_relative(a_source);
				const bool directory = it->is_directory(ec);
				if (ec) break;
				if (directory) {
					a_snapshot.directories.push_back(relative);
					continue;
				}

				const bool regular = it->is_regular_file(ec);
				if (ec) break;
				if (!regular) {
					a_error = Utf8Path(relative) + ": unsupported filesystem entry";
					return false;
				}
				if (a_skip && a_skip(it->path())) {
					continue;
				}

				const auto size = it->file_size(ec);
				if (ec) break;
				const auto modified = it->last_write_time(ec);
				if (ec) break;
				a_snapshot.files.push_back({
					.source = it->path(),
					.relativePath = relative,
					.relative = Utf8Path(relative),
					.size = size,
					.modified = static_cast<std::int64_t>(modified.time_since_epoch().count()),
				});
			}
			if (ec) {
				return Fail(a_error, a_source, ec);
			}

			std::ranges::sort(a_snapshot.files, {}, &FileStamp::relative);
			return true;
		}

		// Metadata only: no file content is read.
		Fingerprint FingerprintSnapshot(const TreeSnapshot& a_snapshot, std::string_view a_salt)
		{
			Fingerprint result;
			result.value = Mix(kFnvOffset, kCacheFormat);
			MixText(result.value, a_salt);
			for (const auto& file : a_snapshot.files) {
				MixText(result.value, file.relative);
				result.value = Mix(result.value, file.size);
				result.value = Mix(result.value, static_cast<std::uint64_t>(file.modified));
				result.bytes += file.size;
			}
			result.files = a_snapshot.files.size();
			return result;
		}

		// Named generations encode the fingerprint in their path; stable mod folders store it in the marker.
		bool IsComplete(const std::filesystem::path& a_generation, std::string_view a_fingerprint = {})
		{
			std::error_code ec;
			if (!std::filesystem::is_regular_file(a_generation / kCompleteMarker, ec)) return false;
			if (a_fingerprint.empty()) return true;
			std::ifstream in(a_generation / kCompleteMarker, std::ios::binary);
			std::string stored;
			std::getline(in, stored);
			return !in.bad() && stored == a_fingerprint;
		}

		bool WriteMarker(const std::filesystem::path& a_path, std::string_view a_fingerprint = {})
		{
			std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
			out << a_fingerprint;
			out.close();
			return out.good();
		}

		// Keep the old entry until the new snapshot is complete, and restore it if publication fails.
		bool ReplaceEntry(const std::filesystem::path& a_staging, const std::filesystem::path& a_destination,
			std::string& a_error)
		{
			auto previous = a_staging;
			previous += "-previous";
			std::error_code ec;
			std::filesystem::remove_all(previous, ec);
			if (ec) return Fail(a_error, previous, ec);
			const bool hadPrevious = std::filesystem::exists(a_destination, ec);
			if (ec) return Fail(a_error, a_destination, ec);
			if (hadPrevious) {
				std::filesystem::rename(a_destination, previous, ec);
				if (ec) return Fail(a_error, a_destination, ec);
			}
			std::filesystem::rename(a_staging, a_destination, ec);
			if (!ec) return true;  // Scavenge removes the previous snapshot after the complete root is ready.
			Fail(a_error, a_destination, ec);
			if (hadPrevious) {
				std::error_code restoreError;
				std::filesystem::rename(previous, a_destination, restoreError);
				if (restoreError) a_error += "; could not restore previous cache: " + restoreError.message();
			}
			return false;
		}

		std::string SafeStagingId(std::string_view a_value)
		{
			std::string out;
			out.reserve(a_value.size());
			for (const unsigned char ch : a_value) {
				out.push_back((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
							  (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'
						  ? static_cast<char>(ch)
						  : '_');
			}
			return out.empty() ? "unknown" : out;
		}

		// Streamed (the copy path already proven under USVFS) so a size change since the snapshot is caught.
		bool CopyCachedFile(const FileStamp& a_file, const std::filesystem::path& a_destination, std::string& a_error)
		{
			std::ifstream source(a_file.source, std::ios::binary);
			if (!source) {
				a_error = a_file.relative + ": could not read source file";
				return false;
			}
			std::ofstream destination(a_destination, std::ios::binary | std::ios::trunc);
			if (!destination) {
				a_error = a_file.relative + ": could not create cached file";
				return false;
			}

			std::array<char, 64 * 1024> buffer;
			std::uintmax_t bytesRead = 0;
			const auto capacity = static_cast<std::streamsize>(buffer.size());
			for (;;) {
				source.read(buffer.data(), capacity);
				const auto count = source.gcount();
				if (count > 0 && !destination.write(buffer.data(), count)) {
					a_error = a_file.relative + ": could not write cached file";
					return false;
				}
				bytesRead += static_cast<std::uintmax_t>(count);
				if (count != capacity) {
					break;
				}
			}
			if (source.bad() || !source.eof() || bytesRead != a_file.size) {
				a_error = a_file.relative + ": source file changed while copying";
				return false;
			}

			destination.close();
			if (!destination) {
				a_error = a_file.relative + ": could not finish cached file";
				return false;
			}
			return true;
		}

		bool CopyTree(const TreeSnapshot& a_snapshot, const std::filesystem::path& a_destination, std::string& a_error)
		{
			std::error_code ec;
			std::filesystem::create_directories(a_destination, ec);
			if (ec) {
				return Fail(a_error, a_destination, ec);
			}
			for (const auto& relative : a_snapshot.directories) {
				std::filesystem::create_directories(a_destination / relative, ec);
				if (ec) {
					return Fail(a_error, a_destination / relative, ec);
				}
			}
			// Every parent was created above. Keep small trees serial; bound large-tree
			// copying so per-file opens/closes can overlap without flooding disk/USVFS.
			constexpr std::size_t kParallelCopyThreshold = 32;
			const std::size_t workerCount = a_snapshot.files.size() >= kParallelCopyThreshold ? 4 : 1;
			std::atomic_size_t next{ 0 };
			std::atomic_bool failed{ false };
			const auto reportFailure = [&](std::string error) {
				// Only the first failing worker writes; the caller reads after all joins.
				if (!failed.exchange(true, std::memory_order_relaxed)) a_error = std::move(error);
			};
			const auto copy = [&] {
				try {
					while (!failed.load(std::memory_order_relaxed)) {
						const auto index = next.fetch_add(1, std::memory_order_relaxed);
						if (index >= a_snapshot.files.size()) return;
						const auto& file = a_snapshot.files[index];
						std::string error;
						if (!CopyCachedFile(file, a_destination / file.relativePath, error)) {
							reportFailure(std::move(error));
							return;
						}
					}
				} catch (const std::exception& error) {
					reportFailure(error.what());
				}
			};
			{
				std::vector<std::jthread> workers;
				workers.reserve(workerCount - 1);
				for (std::size_t i = 1; i < workerCount; ++i) workers.emplace_back(copy);
				copy();
			}
			return !failed.load(std::memory_order_relaxed);
		}
	}  // namespace

	std::optional<Fingerprint> FingerprintTree(const std::filesystem::path& a_source, std::string_view a_salt, std::string& a_error, const SkipFile& a_skip)
	{
		a_error.clear();
		TreeSnapshot snapshot;
		if (!SnapshotTree(a_source, snapshot, a_error, a_skip)) {
			return std::nullopt;
		}
		return FingerprintSnapshot(snapshot, a_salt);
	}

	std::string GenerationName(std::uint64_t a_fingerprint)
	{
		return std::format("{}{:016x}", kGenerationPrefix, a_fingerprint);
	}

	static std::optional<Prepared> PrepareTree(const std::filesystem::path& a_source, const std::filesystem::path& a_cacheRoot,
		std::string_view a_salt, std::string_view a_stagingId, std::string& a_error, const std::filesystem::path& a_destination = {})
	{
		using Clock = std::chrono::steady_clock;
		const auto elapsedMs = [](Clock::time_point start) {
			return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
		};
		const auto scanStarted = Clock::now();
		a_error.clear();
		TreeSnapshot snapshot;
		if (!SnapshotTree(a_source, snapshot, a_error)) {
			return std::nullopt;
		}
		const auto fingerprint = FingerprintSnapshot(snapshot, a_salt);
		Prepared prepared{ .fingerprint = fingerprint, .scanMs = elapsedMs(scanStarted) };

		std::error_code ec;
		std::filesystem::create_directories(a_cacheRoot, ec);
		if (ec) {
			Fail(a_error, a_cacheRoot, ec);
			return std::nullopt;
		}

		const auto fingerprintName = GenerationName(fingerprint.value);
		const auto generation = a_destination.empty() ? a_cacheRoot / fingerprintName : a_destination;
		const std::string_view marker = a_destination.empty() ? std::string_view{} : fingerprintName;
		prepared.generation = generation;
		if (IsComplete(generation, marker)) {
			prepared.reused = true;
			return prepared;
		}

		if (a_destination.empty() && std::filesystem::exists(generation, ec)) {
			if (ec) {
				Fail(a_error, generation, ec);
				return std::nullopt;
			}
			std::filesystem::remove_all(generation, ec);
			if (ec) {
				Fail(a_error, generation, ec);
				return std::nullopt;
			}
		}

		const auto staging = a_cacheRoot / (std::string(kStagingPrefix) + SafeStagingId(a_stagingId));
		std::filesystem::remove_all(staging, ec);
		if (ec) {
			Fail(a_error, staging, ec);
			return std::nullopt;
		}
		const auto cleanupStaging = [&] {
			std::error_code ignored;
			std::filesystem::remove_all(staging, ignored);
		};

		const auto copyStarted = Clock::now();
		if (!CopyTree(snapshot, staging, a_error)) {
			cleanupStaging();
			return std::nullopt;
		}
		prepared.copyMs = elapsedMs(copyStarted);
		// A file added, removed or rewritten during the copy changes the metadata; never publish a mixed tree under the old name.
		const auto verifyStarted = Clock::now();
		const auto after = FingerprintTree(a_source, a_salt, a_error);
		prepared.verifyMs = elapsedMs(verifyStarted);
		if (!after || after->value != fingerprint.value) {
			if (a_error.empty()) {
				a_error = "source tree changed while publishing the cache generation";
			}
			cleanupStaging();
			return std::nullopt;
		}

		if (!WriteMarker(staging / kCompleteMarker, marker)) {
			a_error = std::string(kCompleteMarker) + ": could not write completion marker";
			cleanupStaging();
			return std::nullopt;
		}

		if (!a_destination.empty()) {
			if (!ReplaceEntry(staging, generation, a_error)) {
				cleanupStaging();
				return std::nullopt;
			}
			return prepared;
		}
		std::filesystem::rename(staging, generation, ec);
		if (ec) {
			// Another process may have published the identical generation first.
			if (IsComplete(generation)) {
				cleanupStaging();
				prepared.reused = true;
				return prepared;
			}
			Fail(a_error, generation, ec);
			cleanupStaging();
			return std::nullopt;
		}
		return prepared;
	}

	std::optional<Prepared> Prepare(const std::filesystem::path& a_source, const std::filesystem::path& a_cacheRoot,
		std::string_view a_salt, std::string_view a_stagingId, std::string& a_error)
	{
		return PrepareTree(a_source, a_cacheRoot, a_salt, a_stagingId, a_error);
	}

	std::optional<PreparedMods> PrepareMods(const std::filesystem::path& a_source, const std::filesystem::path& a_cacheRoot,
		std::string_view a_salt, std::string_view a_stagingId, std::string& a_error)
	{
		a_error.clear();
		PreparedMods prepared{ .root = a_cacheRoot / "mods" };
		std::error_code ec;
		std::vector<std::filesystem::directory_entry> entries;
		for (std::filesystem::directory_iterator it(a_source, ec), end; !ec && it != end; it.increment(ec)) {
			entries.push_back(*it);
		}
		if (ec) { Fail(a_error, a_source, ec); return std::nullopt; }
		std::ranges::sort(entries, {}, &std::filesystem::directory_entry::path);
		std::filesystem::create_directories(prepared.root, ec);
		if (ec) { Fail(a_error, prepared.root, ec); return std::nullopt; }
		for (const auto& entry : entries) {
			const auto name = entry.path().filename();
			const auto destination = prepared.root / name;
			// The ordinal is unique even when mod names contain punctuation or Unicode.
			const auto stagingId = std::format("{}-{}", a_stagingId, prepared.entries.size());
			if (entry.is_directory(ec)) {
				const auto mod = PrepareTree(entry.path(), a_cacheRoot, a_salt, stagingId, a_error, destination);
				if (!mod) return std::nullopt;
				prepared.entries.push_back({ Utf8Path(name), *mod });
				continue;
			}
			if (ec) { Fail(a_error, entry.path(), ec); return std::nullopt; }
			if (!entry.is_regular_file(ec) || ec) {
				a_error = Utf8Path(name) + ": unsupported filesystem entry";
				return std::nullopt;
			}
			const auto size = entry.file_size(ec);
			if (ec) { Fail(a_error, entry.path(), ec); return std::nullopt; }
			const auto modified = entry.last_write_time(ec);
			if (ec) { Fail(a_error, entry.path(), ec); return std::nullopt; }
			Prepared file{ .generation = destination, .fingerprint = { .bytes = size, .files = 1 } };
			file.reused = std::filesystem::is_regular_file(destination, ec) && !ec &&
				std::filesystem::file_size(destination, ec) == size && !ec &&
				std::filesystem::last_write_time(destination, ec) == modified && !ec;
			ec.clear();
			if (!file.reused) {
				const auto started = std::chrono::steady_clock::now();
				const auto staging = a_cacheRoot / (std::string(kStagingPrefix) + SafeStagingId(stagingId));
				std::filesystem::remove_all(staging, ec);
				if (ec) { Fail(a_error, staging, ec); return std::nullopt; }
				if (!CopyCachedFile({ .source = entry.path(), .relative = Utf8Path(name), .size = size }, staging, a_error)) return std::nullopt;
				if (std::filesystem::last_write_time(entry.path(), ec) != modified || ec ||
					std::filesystem::file_size(entry.path(), ec) != size || ec) {
					a_error = Utf8Path(name) + ": source file changed while copying";
					return std::nullopt;
				}
				std::filesystem::last_write_time(staging, modified, ec);
				if (ec) { Fail(a_error, staging, ec); return std::nullopt; }
				if (!ReplaceEntry(staging, destination, a_error)) return std::nullopt;
				file.copyMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
			}
			prepared.entries.push_back({ Utf8Path(name), file });
		}
		// Removing a mod must also remove its browser-visible files. Only this owned cache root is pruned.
		for (std::filesystem::directory_iterator it(prepared.root, ec), end; !ec && it != end; it.increment(ec)) {
			bool keep = false;
			for (const auto& mod : prepared.entries) {
				// File identity also handles case-only mod renames on Windows.
				keep = std::filesystem::equivalent(it->path(), mod.cache.generation, ec);
				if (ec) { Fail(a_error, it->path(), ec); return std::nullopt; }
				if (keep) break;
			}
			if (keep) continue;
			std::filesystem::remove_all(it->path(), ec);
			if (ec) break;
			++prepared.removed;
		}
		if (ec) { Fail(a_error, prepared.root, ec); return std::nullopt; }
		return prepared;
	}

	ScavengeResult Scavenge(const std::filesystem::path& a_cacheRoot, const std::filesystem::path& a_keep)
	{
		ScavengeResult result;
		std::error_code ec;
		if (!std::filesystem::is_directory(a_cacheRoot, ec) || ec) {
			return result;
		}
		for (std::filesystem::directory_iterator it(a_cacheRoot, ec), end;
			 !ec && it != end; it.increment(ec)) {
			const auto name = Utf8Path(it->path().filename());
			if (!name.starts_with(kGenerationPrefix) && !name.starts_with(kStagingPrefix)) {
				continue;
			}
			if (!a_keep.empty() && it->path().lexically_normal() == a_keep.lexically_normal()) {
				continue;
			}
			std::error_code removeEc;
			std::filesystem::remove_all(it->path(), removeEc);
			if (removeEc) {
				++result.failed;
			} else {
				++result.removed;
			}
		}
		if (ec) {
			++result.failed;
		}
		return result;
	}
}
