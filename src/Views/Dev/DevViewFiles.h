#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace OSFUI::DevViewFiles
{
	// Fingerprint the mod's files, excluding restart-only manifests.
	[[nodiscard]] std::optional<std::uint64_t> Fingerprint(const std::filesystem::path& a_viewDir);

	// Copy/update before pruning stale entries; report failures so callers can retry safely.
	[[nodiscard]] bool SyncTree(const std::filesystem::path& a_source, const std::filesystem::path& a_destination,
		std::string& a_error);
}  // namespace OSFUI::DevViewFiles
