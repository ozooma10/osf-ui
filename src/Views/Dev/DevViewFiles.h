#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace OSFUI::DevViewFiles
{
	// Return the whole mod scope because a view entry may load sibling assets and shared scripts.
	[[nodiscard]] std::string ModFolder(std::string_view a_viewId);

	// Fingerprint sorted mod files excluding restart-only manifests.
	[[nodiscard]] std::optional<std::uint64_t> Fingerprint(const std::filesystem::path& a_viewDir);

	// Copy/update before pruning stale entries; report failures so callers can retry safely.
	[[nodiscard]] bool SyncTree(const std::filesystem::path& a_source, const std::filesystem::path& a_destination,
		std::string& a_error);
}  // namespace OSFUI::DevViewFiles
