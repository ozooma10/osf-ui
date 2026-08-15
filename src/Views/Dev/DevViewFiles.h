#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace OSFUI::DevViewFiles
{
	// The mod folder segment of a qualified view id ("<modId>/<viewName>" -> "<modId>").
	//
	// A view's real footprint is its whole mod folder, never just
	// views/<modId>/<viewName>: the bundler emits hashed chunks to a mod-level
	// sibling (views/<modId>/assets/) that the entry HTML reaches through
	// "../assets/...", built-ins keep shared scripts there too (osfui/padnav.js),
	// and `osfui dev` deploys views/<modId> as one unit. Fingerprint and mirror
	// at this scope — watching only the view folder sees the rewritten
	// index.html but copies none of the bundles it now points at, so the reload
	// lands on ERR_FILE_NOT_FOUND for a hashed chunk that was never mirrored.
	[[nodiscard]] std::string ModFolder(std::string_view a_viewId);

	// Deterministic metadata fingerprint for one authored mod folder. Files are
	// sorted by relative path before hashing, so filesystem enumeration order
	// cannot manufacture a change. Manifests are excluded at every depth because
	// native manifest discovery remains restart-only.
	[[nodiscard]] std::optional<std::uint64_t> Fingerprint(const std::filesystem::path& a_viewDir);

	// Make a_destination an exact recursive copy of a_source: update/add first,
	// then remove destination entries absent from the source. Returns false and
	// fills a_error on any filesystem failure so the caller can retry after an
	// editor or antivirus process releases a file.
	[[nodiscard]] bool SyncTree(const std::filesystem::path& a_source, const std::filesystem::path& a_destination,
		std::string& a_error);
}  // namespace OSFUI::DevViewFiles
