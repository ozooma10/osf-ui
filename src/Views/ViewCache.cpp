#include "Views/ViewCache.h"

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
		constexpr std::uint64_t kCacheFormat = 1;

		struct FileStamp
		{
			std::filesystem::path source;
			std::filesystem::path relativePath;
			std::string           relative;
			std::uintmax_t        size{ 0 };
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

		void MixBytes(std::uint64_t& a_hash, std::span<const char> a_bytes)
		{
			for (const unsigned char byte : a_bytes) {
				a_hash = Mix(a_hash, byte);
			}
		}

		void MixText(std::uint64_t& a_hash, std::string_view a_text)
		{
			MixBytes(a_hash, { a_text.data(), a_text.size() });
			a_hash = Mix(a_hash, 0);
		}

		template <class Consumer>
		bool ReadFile(const FileStamp& a_file, Consumer&& a_consumer, std::string& a_error)
		{
			std::ifstream stream(a_file.source, std::ios::binary);
			if (!stream) {
				a_error = a_file.relative + ": could not read source file";
				return false;
			}

			std::array<char, 64 * 1024> buffer;
			std::uintmax_t bytesRead = 0;
			const auto capacity = static_cast<std::streamsize>(buffer.size());
			for (;;) {
				stream.read(buffer.data(), capacity);
				const auto count = stream.gcount();
				if (count > 0 && !a_consumer(std::span<const char>{
						buffer.data(), static_cast<std::size_t>(count) })) {
					return false;
				}
				bytesRead += static_cast<std::uintmax_t>(count);
				if (count != capacity) {
					break;
				}
			}
			if (stream.bad() || !stream.eof() || bytesRead != a_file.size) {
				a_error = a_file.relative + ": source file changed while fingerprinting";
				return false;
			}
			return true;
		}

		bool MixFile(std::uint64_t& a_hash, const FileStamp& a_file, std::string& a_error)
		{
			return ReadFile(a_file,
				[&](std::span<const char> a_bytes) {
					MixBytes(a_hash, a_bytes);
					return true;
				},
				a_error);
		}

		bool Fail(std::string& a_error, const std::filesystem::path& a_path, const std::error_code& a_ec)
		{
			a_error = a_path.filename().string() + ": " + a_ec.message();
			return false;
		}

		bool SnapshotTree(const std::filesystem::path& a_source,
			TreeSnapshot& a_snapshot, std::string& a_error)
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
					a_error = relative.generic_string() + ": unsupported filesystem entry";
					return false;
				}

				const auto size = it->file_size(ec);
				if (ec) break;
				a_snapshot.files.push_back({
					.source = it->path(),
					.relativePath = relative,
					.relative = relative.generic_string(),
					.size = size,
				});
			}
			if (ec) {
				return Fail(a_error, a_source, ec);
			}

			std::ranges::sort(a_snapshot.files, {}, &FileStamp::relative);
			return true;
		}

		Fingerprint BeginFingerprint(std::string_view a_salt)
		{
			Fingerprint result;
			result.value = Mix(kFnvOffset, kCacheFormat);
			MixText(result.value, a_salt);
			return result;
		}

		std::optional<Fingerprint> FingerprintFiles(const std::vector<FileStamp>& a_files,
			std::string_view a_salt, std::string& a_error)
		{
			auto result = BeginFingerprint(a_salt);
			for (const auto& file : a_files) {
				MixText(result.value, file.relative);
				result.value = Mix(result.value, file.size);
				if (!MixFile(result.value, file, a_error)) {
					return std::nullopt;
				}
				result.bytes += file.size;
			}
			result.files = a_files.size();
			return result;
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

		bool CopyFileAndMix(std::uint64_t& a_hash, const FileStamp& a_file,
			const std::filesystem::path& a_destination, std::string& a_error)
		{
			std::ofstream destination(a_destination, std::ios::binary | std::ios::trunc);
			if (!destination) {
				a_error = a_file.relative + ": could not create cached file";
				return false;
			}

			if (!ReadFile(a_file,
					[&](std::span<const char> a_bytes) {
						destination.write(a_bytes.data(),
							static_cast<std::streamsize>(a_bytes.size()));
						if (!destination) {
							a_error = a_file.relative + ": could not write cached file";
							return false;
						}
						MixBytes(a_hash, a_bytes);
						return true;
					},
					a_error)) {
				return false;
			}

			destination.close();
			if (!destination) {
				a_error = a_file.relative + ": could not finish cached file";
				return false;
			}
			return true;
		}

		std::optional<Fingerprint> CopyTreeAndFingerprint(
			const std::filesystem::path& a_source,
			const std::filesystem::path& a_destination, std::string_view a_salt,
			std::string& a_error)
		{
			TreeSnapshot snapshot;
			if (!SnapshotTree(a_source, snapshot, a_error)) {
				return std::nullopt;
			}

			std::error_code ec;
			std::filesystem::create_directories(a_destination, ec);
			if (ec) {
				Fail(a_error, a_destination, ec);
				return std::nullopt;
			}
			for (const auto& relative : snapshot.directories) {
				std::filesystem::create_directories(a_destination / relative, ec);
				if (ec) {
					Fail(a_error, a_destination / relative, ec);
					return std::nullopt;
				}
			}

			auto result = BeginFingerprint(a_salt);
			for (const auto& file : snapshot.files) {
				MixText(result.value, file.relative);
				result.value = Mix(result.value, file.size);

				const auto destination = a_destination / file.relativePath;
				std::filesystem::create_directories(destination.parent_path(), ec);
				if (ec) {
					Fail(a_error, destination.parent_path(), ec);
					return std::nullopt;
				}
				if (!CopyFileAndMix(result.value, file, destination, a_error)) {
					return std::nullopt;
				}
				result.bytes += file.size;
			}
			result.files = snapshot.files.size();
			return result;
		}
	}  // namespace

	std::optional<Fingerprint> FingerprintTree(const std::filesystem::path& a_source, std::string_view a_salt, std::string& a_error)
	{
		a_error.clear();
		TreeSnapshot snapshot;
		if (!SnapshotTree(a_source, snapshot, a_error)) {
			return std::nullopt;
		}
		return FingerprintFiles(snapshot.files, a_salt, a_error);
	}

	std::string GenerationName(std::uint64_t a_fingerprint)
	{
		return std::format("{}{:016x}", kGenerationPrefix, a_fingerprint);
	}

	std::optional<Prepared> Prepare(const std::filesystem::path& a_source, const std::filesystem::path& a_cacheRoot, std::string_view a_salt, std::string_view a_stagingId, std::string& a_error)
	{
		a_error.clear();
		const auto fingerprint = FingerprintTree(a_source, a_salt, a_error);
		if (!fingerprint) {
			return std::nullopt;
		}

		std::error_code ec;
		std::filesystem::create_directories(a_cacheRoot, ec);
		if (ec) {
			Fail(a_error, a_cacheRoot, ec);
			return std::nullopt;
		}

		const auto generation = a_cacheRoot / GenerationName(fingerprint->value);
		if (IsComplete(generation, *fingerprint, a_salt)) {
			return Prepared{ generation, *fingerprint, true };
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

		const auto copiedFingerprint =
			CopyTreeAndFingerprint(a_source, staging, a_salt, a_error);
		if (!copiedFingerprint || copiedFingerprint->value != fingerprint->value ||
			copiedFingerprint->files != fingerprint->files ||
			copiedFingerprint->bytes != fingerprint->bytes) {
			if (a_error.empty()) {
				a_error = "source tree changed while publishing the cache generation";
			}
			cleanupStaging();
			return std::nullopt;
		}

		{
			std::ofstream lock(staging / kUseLock, std::ios::binary | std::ios::trunc);
			if (!lock) {
				a_error = std::string(kUseLock) + ": could not create cache lease file";
				cleanupStaging();
				return std::nullopt;
			}
		}
		{
			std::ofstream marker(staging / kCompleteMarker, std::ios::binary | std::ios::trunc);
			if (!marker) {
				a_error = std::string(kCompleteMarker) + ": could not create completion marker";
				cleanupStaging();
				return std::nullopt;
			}
			marker << MarkerText(*fingerprint, a_salt);
			if (!marker.good()) {
				a_error = std::string(kCompleteMarker) + ": could not write completion marker";
				marker.close();
				cleanupStaging();
				return std::nullopt;
			}
		}

		std::filesystem::rename(staging, generation, ec);
		if (ec) {
			// A serialized caller should not race, but accepting an independently published identical generation makes the helper robust on its own.
			if (IsComplete(generation, *fingerprint, a_salt)) {
				cleanupStaging();
				return Prepared{ generation, *fingerprint, true };
			}
			Fail(a_error, generation, ec);
			cleanupStaging();
			return std::nullopt;
		}
		return Prepared{ generation, *fingerprint, false };
	}

	ScavengeResult Scavenge(const std::filesystem::path& a_cacheRoot, const std::filesystem::path& a_keep, const CanRemove& a_canRemove)
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
			const auto name = it->path().filename().string();
			if (!name.starts_with(kGenerationPrefix) && !name.starts_with(kStagingPrefix)) {
				continue;
			}
			if (!a_keep.empty() && it->path().lexically_normal() == a_keep.lexically_normal()) {
				++result.retained;
				continue;
			}
			if (a_canRemove && !a_canRemove(it->path())) {
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
