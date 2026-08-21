#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace OSFUI::ViewCache
{
	inline constexpr std::string_view kCacheDirectory = "views-cache";
	inline constexpr std::string_view kGenerationPrefix = "gen-";
	inline constexpr std::string_view kStagingPrefix = "staging-";
	inline constexpr std::string_view kCompleteMarker = ".osfui-cache-complete";
	inline constexpr std::string_view kUseLock = ".osfui-cache-use.lock";
	inline constexpr wchar_t kMutexName[] = L"Local\\OSFUI-ViewsCache-v1";

	struct Fingerprint
	{
		std::uint64_t value{ 0 };
		std::uint64_t bytes{ 0 };
		std::size_t   files{ 0 };
	};

	struct Prepared
	{
		std::filesystem::path generation;
		Fingerprint           fingerprint;
		bool                  reused{ false };
	};

	struct ScavengeResult
	{
		std::size_t removed{ 0 };
		std::size_t retained{ 0 };
		std::size_t failed{ 0 };
	};

	using CanRemove = std::function<bool(const std::filesystem::path&)>;

	// Content fingerprint of the complete resolved USVFS tree. The salt carries cache-format/runtime compatibility so a release can invalidate old snapshots.
	[[nodiscard]] std::optional<Fingerprint> FingerprintTree(const std::filesystem::path& a_source, std::string_view a_salt, std::string& a_error);

	[[nodiscard]] std::string GenerationName(std::uint64_t a_fingerprint);

	// Each virtual host maps to one isolated mod directory. Materialize the canonical root shared/ tree beneath every mod so /shared/* stays on that mod's origin without exposing sibling mods through the mapping.
	[[nodiscard]] bool MaterializeSharedAssets(const std::filesystem::path& a_viewsRoot, std::string& a_error);

	// Reuse a complete immutable generation, or copy into a private staging tree and atomically publish it. Callers serialize this across processes.
	[[nodiscard]] std::optional<Prepared> Prepare(const std::filesystem::path& a_source, const std::filesystem::path& a_cacheRoot, std::string_view a_salt, std::string_view a_stagingId, std::string& a_error);

	// Remove abandoned staging trees and old generations. The keep generation is never considered; CanRemove lets the Windows caller honor active lease files.
	[[nodiscard]] ScavengeResult Scavenge(const std::filesystem::path& a_cacheRoot, const std::filesystem::path& a_keep, const CanRemove& a_canRemove = {});
} 
