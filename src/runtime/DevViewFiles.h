#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace OSFUI::DevViewFiles
{
	// Deterministic metadata fingerprint for one authored view. Files are sorted
	// by relative path before hashing, so filesystem enumeration order cannot
	// manufacture a change. The root manifest is excluded because native
	// manifest discovery remains restart-only.
	[[nodiscard]] std::optional<std::uint64_t> Fingerprint(const std::filesystem::path& a_viewDir);

	// Make a_destination an exact recursive copy of a_source: update/add first,
	// then remove destination entries absent from the source. Returns false and
	// fills a_error on any filesystem failure so the caller can retry after an
	// editor or antivirus process releases a file.
	[[nodiscard]] bool SyncTree(const std::filesystem::path& a_source, const std::filesystem::path& a_destination,
		std::string& a_error);
}  // namespace OSFUI::DevViewFiles
