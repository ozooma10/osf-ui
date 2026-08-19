#include "Views/ViewCache.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
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
			std::string           relative;
			std::uintmax_t        size{ 0 };
		};

		std::uint64_t Mix(std::uint64_t a_hash, std::uint64_t a_value)
		{
			return (a_hash ^ a_value) * kFnvPrime;
		}

		void MixText(std::uint64_t& a_hash, std::string_view a_text)
		{
			for (const unsigned char ch : a_text) {
				a_hash = Mix(a_hash, ch);
			}
			a_hash = Mix(a_hash, 0);
		}

		bool MixFile(std::uint64_t& a_hash, const FileStamp& a_file, std::string& a_error)
		{
			std::ifstream stream(a_file.source, std::ios::binary);
			if (!stream) {
				a_error = a_file.relative + ": could not read source file";
				return false;
			}
			std::array<char, 64 * 1024> buffer{};
			std::uintmax_t bytesRead = 0;
			while (stream) {
				stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
				const auto count = stream.gcount();
				for (std::streamsize i = 0; i < count; ++i) {
					a_hash = Mix(a_hash, static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]));
				}
				bytesRead += static_cast<std::uintmax_t>(count);
			}
			if (stream.bad() || bytesRead != a_file.size) {
				a_error = a_file.relative + ": source file changed while fingerprinting";
				return false;
			}
			return true;
		}

		bool Fail(std::string& a_error, const std::filesystem::path& a_path, const std::error_code& a_ec)
		{
			a_error = a_path.filename().string() + ": " + a_ec.message();
			return false;
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

		bool CopyTree(const std::filesystem::path& a_source, const std::filesystem::path& a_destination, std::string& a_error)
		{
			std::error_code ec;
			std::filesystem::create_directories(a_destination, ec);
			if (ec) {
				return Fail(a_error, a_destination, ec);
			}

			for (std::filesystem::recursive_directory_iterator it(a_source, ec), end; !ec && it != end; it.increment(ec)) {
				const auto relative = it->path().lexically_relative(a_source);
				const auto destination = a_destination / relative;
				if (it->is_directory(ec)) {
					if (ec) break;
					std::filesystem::create_directories(destination, ec);
					if (ec) break;
					continue;
				}
				if (!it->is_regular_file(ec)) {
					if (ec) break;
					a_error = relative.generic_string() + ": unsupported filesystem entry";
					return false;
				}
				std::filesystem::create_directories(destination.parent_path(), ec);
				if (ec) break;
				std::filesystem::copy_file(it->path(), destination, std::filesystem::copy_options::overwrite_existing, ec);
				if (ec) break;
			}
			if (ec) {
				return Fail(a_error, a_source, ec);
			}
			return true;
		}
	}  // namespace

	std::optional<Fingerprint> FingerprintTree(const std::filesystem::path& a_source, std::string_view a_salt, std::string& a_error)
	{
		a_error.clear();
		std::error_code ec;
		if (!std::filesystem::is_directory(a_source, ec) || ec) {
			a_error = ec ? ec.message() : "source is not a directory";
			return std::nullopt;
		}

		std::vector<FileStamp> files;
		for (std::filesystem::recursive_directory_iterator it(a_source, ec), end;
			 !ec && it != end; it.increment(ec)) {
			if (it->is_directory(ec)) {
				if (ec) break;
				continue;
			}
			if (!it->is_regular_file(ec)) {
				if (ec) break;
				a_error = it->path().lexically_relative(a_source).generic_string() +
					": unsupported filesystem entry";
				return std::nullopt;
			}
			const auto size = it->file_size(ec);
			if (ec) break;
			files.push_back({
				.source = it->path(),
				.relative = it->path().lexically_relative(a_source).generic_string(),
				.size = size,
			});
		}
		if (ec) {
			Fail(a_error, a_source, ec);
			return std::nullopt;
		}

		std::ranges::sort(files, {}, &FileStamp::relative);
		Fingerprint result;
		result.value = kFnvOffset;
		result.value = Mix(result.value, kCacheFormat);
		MixText(result.value, a_salt);
		for (const auto& file : files) {
			MixText(result.value, file.relative);
			result.value = Mix(result.value, file.size);
			if (!MixFile(result.value, file, a_error)) {
				return std::nullopt;
			}
			result.bytes += file.size;
		}
		result.files = files.size();
		return result;
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

		if (!CopyTree(a_source, staging, a_error)) {
			cleanupStaging();
			return std::nullopt;
		}
		const auto stagedFingerprint = FingerprintTree(staging, a_salt, a_error);
		if (!stagedFingerprint || stagedFingerprint->value != fingerprint->value || stagedFingerprint->files != fingerprint->files || stagedFingerprint->bytes != fingerprint->bytes) {
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
