#include "Views/ViewCache.h"
#include "Core/Utf8Path.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <span>
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

		bool SameFingerprint(const Fingerprint& a_left, const Fingerprint& a_right)
		{
			return a_left.value == a_right.value && a_left.files == a_right.files && a_left.bytes == a_right.bytes;
		}

		std::string MarkerText(const Fingerprint& a_fingerprint, std::string_view a_salt)
		{
			return std::format("format={}\nfingerprint={:016x}\nfiles={}\nbytes={}\nsalt={}\n", kCacheFormat, a_fingerprint.value, a_fingerprint.files, a_fingerprint.bytes, a_salt);
		}

		bool IsComplete(const std::filesystem::path& a_generation,
			const Fingerprint& a_fingerprint, std::string_view a_salt)
		{
			std::ifstream marker(a_generation / kCompleteMarker, std::ios::binary);
			if (!marker) {
				return false;
			}
			const std::string content{ std::istreambuf_iterator<char>(marker), {} };
			return content == MarkerText(a_fingerprint, a_salt);
		}

		bool WriteText(const std::filesystem::path& a_path, std::string_view a_text)
		{
			std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
			if (!out) return false;
			out << a_text;
			out.close();
			return out.good();
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
			for (const auto& file : a_snapshot.files) {
				const auto destination = a_destination / file.relativePath;
				std::filesystem::create_directories(destination.parent_path(), ec);
				if (ec) {
					return Fail(a_error, destination.parent_path(), ec);
				}
				if (!CopyCachedFile(file, destination, a_error)) {
					return false;
				}
			}
			return true;
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

	std::optional<Prepared> Prepare(const std::filesystem::path& a_source, const std::filesystem::path& a_cacheRoot, std::string_view a_salt, std::string_view a_stagingId, std::string& a_error)
	{
		a_error.clear();
		TreeSnapshot snapshot;
		if (!SnapshotTree(a_source, snapshot, a_error)) {
			return std::nullopt;
		}
		const auto fingerprint = FingerprintSnapshot(snapshot, a_salt);

		std::error_code ec;
		std::filesystem::create_directories(a_cacheRoot, ec);
		if (ec) {
			Fail(a_error, a_cacheRoot, ec);
			return std::nullopt;
		}

		const auto generation = a_cacheRoot / GenerationName(fingerprint.value);
		if (IsComplete(generation, fingerprint, a_salt)) {
			return Prepared{ generation, fingerprint, true };
		}

		if (std::filesystem::exists(generation, ec)) {
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

		if (!CopyTree(snapshot, staging, a_error)) {
			cleanupStaging();
			return std::nullopt;
		}
		// A file added, removed or rewritten during the copy changes the metadata; never publish a mixed tree under the old name.
		const auto after = FingerprintTree(a_source, a_salt, a_error);
		if (!after || !SameFingerprint(*after, fingerprint)) {
			if (a_error.empty()) {
				a_error = "source tree changed while publishing the cache generation";
			}
			cleanupStaging();
			return std::nullopt;
		}

		if (!WriteText(staging / kCompleteMarker, MarkerText(fingerprint, a_salt))) {
			a_error = std::string(kCompleteMarker) + ": could not write completion marker";
			cleanupStaging();
			return std::nullopt;
		}

		std::filesystem::rename(staging, generation, ec);
		if (ec) {
			// Another process may have published the identical generation first.
			if (IsComplete(generation, fingerprint, a_salt)) {
				cleanupStaging();
				return Prepared{ generation, fingerprint, true };
			}
			Fail(a_error, generation, ec);
			cleanupStaging();
			return std::nullopt;
		}
		return Prepared{ generation, fingerprint, false };
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
			if (!it->is_directory(ec)) {
				if (ec) ++result.failed;
				continue;
			}
			const auto name = Utf8Path(it->path().filename());
			if (!name.starts_with(kGenerationPrefix) && !name.starts_with(kStagingPrefix)) {
				continue;
			}
			if (!a_keep.empty() && it->path().lexically_normal() == a_keep.lexically_normal()) {
				++result.retained;
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
