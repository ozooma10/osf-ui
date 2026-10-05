#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OSFUI::ViewCache
{
	inline constexpr std::string_view kCacheDirectory = "views-cache";
	inline constexpr std::string_view kGenerationPrefix = "gen-";
	inline constexpr std::string_view kStagingPrefix = "staging-";
	inline constexpr std::string_view kCompleteMarker = ".osfui-cache-complete";

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
		std::int64_t          scanMs{ 0 };
		std::int64_t          copyMs{ 0 };
		std::int64_t          verifyMs{ 0 };
	};

	struct ScavengeResult
	{
		std::size_t removed{ 0 };
		std::size_t failed{ 0 };
	};

	struct ModPrepared
	{
		std::string name;
		Prepared cache;
	};

	struct PreparedMods
	{
		std::filesystem::path root;
		std::vector<ModPrepared> entries;
		std::size_t removed{ 0 };
	};

	using SkipFile = std::function<bool(const std::filesystem::path&)>;

	// Hash paths, sizes and mtimes through USVFS. Salt invalidates caches; SkipFile excludes files.
	[[nodiscard]] std::optional<Fingerprint> FingerprintTree(const std::filesystem::path& a_source, std::string_view a_salt, std::string& a_error, const SkipFile& a_skip = {});

	[[nodiscard]] std::string GenerationName(std::uint64_t a_fingerprint);

	// Reuse or atomically replace the cache. Same-size edits with unchanged mtimes go undetected.
	[[nodiscard]] std::optional<Prepared> Prepare(const std::filesystem::path& a_source, const std::filesystem::path& a_cacheRoot, std::string_view a_salt, std::string_view a_stagingId, std::string& a_error);

	// Cache each top-level entry separately; verify changes before replacing it.
	// Session preparation only; concurrent game instances are unsupported.
	[[nodiscard]] std::optional<PreparedMods> PrepareMods(const std::filesystem::path& a_source, const std::filesystem::path& a_cacheRoot, std::string_view a_salt, std::string_view a_stagingId, std::string& a_error);

	// Removes all staging entries and generations except a_keep. Not safe across concurrent game instances.
	[[nodiscard]] ScavengeResult Scavenge(const std::filesystem::path& a_cacheRoot, const std::filesystem::path& a_keep);
}
