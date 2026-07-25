#include "runtime/DevViewFiles.h"

#include <algorithm>
#include <vector>

namespace OSFUI::DevViewFiles
{
	namespace
	{
		struct FileStamp
		{
			std::string    relative;
			std::uintmax_t size{ 0 };
			std::uint64_t  writeTime{ 0 };
		};

		std::uint64_t Mix(std::uint64_t a_hash, std::uint64_t a_value)
		{
			constexpr std::uint64_t kPrime = 1099511628211ull;
			return (a_hash ^ a_value) * kPrime;
		}

		bool Fail(std::string& a_error, const std::filesystem::path& a_path, const std::error_code& a_ec)
		{
			a_error = a_path.filename().string() + ": " + a_ec.message();
			return false;
		}
	}  // namespace

	std::optional<std::uint64_t> Fingerprint(const std::filesystem::path& a_viewDir)
	{
		std::error_code ec;
		if (!std::filesystem::is_directory(a_viewDir, ec) || ec) {
			return std::nullopt;
		}

		std::vector<FileStamp> files;
		for (std::filesystem::recursive_directory_iterator
				 it(a_viewDir, std::filesystem::directory_options::skip_permission_denied, ec),
			end;
			!ec && it != end; it.increment(ec)) {
			if (!it->is_regular_file(ec)) {
				if (ec)
					break;
				continue;
			}
			const auto relative = it->path().lexically_relative(a_viewDir).generic_string();
			if (relative == "manifest.json")
				continue;
			const auto size = it->file_size(ec);
			if (ec)
				break;
			const auto writeTime = it->last_write_time(ec);
			if (ec)
				break;
			files.push_back({
				.relative = relative,
				.size = size,
				.writeTime = static_cast<std::uint64_t>(writeTime.time_since_epoch().count()),
			});
		}
		if (ec)
			return std::nullopt;

		std::ranges::sort(files, {}, &FileStamp::relative);
		std::uint64_t hash = 1469598103934665603ull;
		for (const auto& file : files) {
			for (const unsigned char ch : file.relative)
				hash = Mix(hash, ch);
			hash = Mix(hash, 0);
			hash = Mix(hash, file.size);
			hash = Mix(hash, file.writeTime);
		}
		return hash;
	}

	bool SyncTree(const std::filesystem::path& a_source, const std::filesystem::path& a_destination, std::string& a_error)
	{
		a_error.clear();
		std::error_code ec;
		if (!std::filesystem::is_directory(a_source, ec) || ec) {
			a_error = ec ? ec.message() : "source is not a directory";
			return false;
		}

		std::filesystem::create_directories(a_destination, ec);
		if (ec)
			return Fail(a_error, a_destination, ec);

		// Remove stale paths and file/directory type conflicts deepest-first.
		// Recursive copy cannot replace a directory with a file or the reverse.
		std::vector<std::filesystem::path> stale;
		for (std::filesystem::recursive_directory_iterator
				 it(a_destination, std::filesystem::directory_options::skip_permission_denied, ec),
			end;
			!ec && it != end; it.increment(ec)) {
			const auto relative = it->path().lexically_relative(a_destination);
			const auto sourcePeer = a_source / relative;
			const bool sourceExists = std::filesystem::exists(sourcePeer, ec);
			if (ec)
				break;
			const bool destinationDir = it->is_directory(ec);
			if (ec)
				break;
			const bool sourceDir = sourceExists && std::filesystem::is_directory(sourcePeer, ec);
			if (ec)
				break;
			if (!sourceExists || destinationDir != sourceDir) {
				if (ec)
					break;
				stale.push_back(it->path());
			}
		}
		if (ec)
			return Fail(a_error, a_destination, ec);
		std::ranges::sort(stale, [](const auto& a_lhs, const auto& a_rhs) {
			return std::distance(a_lhs.begin(), a_lhs.end()) > std::distance(a_rhs.begin(), a_rhs.end());
		});
		for (const auto& path : stale) {
			std::filesystem::remove_all(path, ec);
			if (ec)
				return Fail(a_error, path, ec);
		}
		std::filesystem::copy(a_source, a_destination,
			std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
			ec);
		if (ec)
			return Fail(a_error, a_source, ec);
		return true;
	}
}  // namespace OSFUI::DevViewFiles
