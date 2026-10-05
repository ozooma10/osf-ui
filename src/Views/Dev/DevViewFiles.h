#pragma once

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace OSFUI::DevViewFiles
{
	// Fingerprint the mod's files, excluding restart-only manifests.
	[[nodiscard]] std::optional<std::uint64_t> Fingerprint(const std::filesystem::path& a_viewDir);

	// Replaces the destination tree. A failed copy leaves a partial tree until the next reload.
	[[nodiscard]] bool ReplaceTree(const std::filesystem::path& a_source, const std::filesystem::path& a_destination,
		std::string& a_error);
}  // namespace OSFUI::DevViewFiles
