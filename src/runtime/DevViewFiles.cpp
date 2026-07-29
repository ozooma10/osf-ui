#include "runtime/DevViewFiles.h"

#include <algorithm>
#include <unordered_set>
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

	std::string ModFolder(std::string_view a_viewId)
	{
		const auto slash = a_viewId.find('/');
		return std::string(slash == std::string_view::npos ? a_viewId : a_viewId.substr(0, slash));
	}

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
			// Any depth: the scope is the mod folder, so every view's manifest
			// sits one level down. Manifest edits stay restart-only.
			if (it->path().filename() == "manifest.json")
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

		struct SourceEntry
		{
			std::filesystem::path source;
			std::filesystem::path relative;
			bool                  directory{ false };
		};
		std::vector<SourceEntry> sourceEntries;
		for (std::filesystem::recursive_directory_iterator
				 it(a_source, std::filesystem::directory_options::skip_permission_denied, ec),
			end;
			!ec && it != end; it.increment(ec)) {
			const bool directory = it->is_directory(ec);
			if (ec)
				break;
			if (!directory && !it->is_regular_file(ec)) {
				if (ec)
					break;
				continue;
			}
			sourceEntries.push_back({
				.source = it->path(),
				.relative = it->path().lexically_relative(a_source),
				.directory = directory,
			});
		}
		if (ec)
			return Fail(a_error, a_source, ec);

		// Deterministic order also puts the conventional assets/ sibling before
		// each view's entry HTML. A rebuilt index can therefore never point at a
		// hashed bundle that has not landed yet.
		std::ranges::sort(sourceEntries, {}, [](const SourceEntry& a_entry) {
			return a_entry.relative.generic_string();
		});

		std::unordered_set<std::string> sourcePaths;
		sourcePaths.reserve(sourceEntries.size());
		for (const auto& entry : sourceEntries) {
			sourcePaths.insert(entry.relative.generic_string());
			const auto destination = a_destination / entry.relative;
			const bool destinationExists = std::filesystem::exists(destination, ec);
			if (ec)
				return Fail(a_error, destination, ec);
			const bool destinationDirectory =
				destinationExists && std::filesystem::is_directory(destination, ec);
			if (ec)
				return Fail(a_error, destination, ec);

			// A file/directory rename needs its old shape removed first, but
			// ordinary stale bundles remain available until every replacement
			// has copied successfully.
			if (destinationExists && destinationDirectory != entry.directory) {
				std::filesystem::remove_all(destination, ec);
				if (ec)
					return Fail(a_error, destination, ec);
			}
			if (entry.directory) {
				std::filesystem::create_directories(destination, ec);
				if (ec)
					return Fail(a_error, destination, ec);
				continue;
			}
			std::filesystem::create_directories(destination.parent_path(), ec);
			if (ec)
				return Fail(a_error, destination.parent_path(), ec);
			std::filesystem::copy_file(entry.source, destination,
				std::filesystem::copy_options::overwrite_existing, ec);
			if (ec)
				return Fail(a_error, entry.source, ec);
		}

		// Only after all current files are safely present may removed/renamed
		// paths disappear. The former stale-first order deleted the browser's
		// working bundle and then left the view disconnected when USVFS rejected
		// the following recursive copy.
		std::vector<std::filesystem::path> stale;
		for (std::filesystem::recursive_directory_iterator
				 it(a_destination, std::filesystem::directory_options::skip_permission_denied, ec),
			end;
			!ec && it != end; it.increment(ec)) {
			const auto relative = it->path().lexically_relative(a_destination);
			if (!sourcePaths.contains(relative.generic_string())) {
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
		return true;
	}
}  // namespace OSFUI::DevViewFiles
