#pragma once

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

namespace OSFUI::ViewMirrorCleanup
{
	struct Result
	{
		std::size_t removed{ 0 };
		std::size_t failed{ 0 };
	};

	// Only the disposable production mirror names shipped in 1.x are owned here.
	// Unknown names (including research instances) and newer caches stay untouched.
	[[nodiscard]] inline std::optional<std::uint32_t> OwnerPid(std::string_view a_name)
	{
		constexpr std::string_view prefix = "views-mirror-";
		if (!a_name.starts_with(prefix)) return std::nullopt;
		const auto digits = a_name.substr(prefix.size());
		if (digits.empty()) return std::nullopt;
		std::uint32_t pid = 0;
		const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), pid);
		if (error != std::errc{} || end != digits.data() + digits.size() || pid == 0) {
			return std::nullopt;
		}
		return pid;
	}

	[[nodiscard]] inline Result Scavenge(const std::filesystem::path& a_root,
		bool a_hostRunning, const std::function<bool(std::uint32_t)>& a_processAlive)
	{
		Result result;
		std::error_code ec;
		if (!std::filesystem::exists(a_root, ec)) {
			result.failed = ec ? 1 : 0;
			return result;
		}
		for (std::filesystem::directory_iterator it(a_root, ec), end;
			!ec && it != end; it.increment(ec)) {
			std::error_code entryEc;
			// Inspect the entry itself so a link cannot redirect cleanup elsewhere.
			const auto status = it->symlink_status(entryEc);
			if (entryEc) {
				++result.failed;
				continue;
			}
			if (!std::filesystem::is_directory(status)) continue;
			const auto name = it->path().filename().string();
			if (name == "views-mirror") {
				if (a_hostRunning) continue;
			} else if (const auto pid = OwnerPid(name)) {
				if (a_processAlive(*pid)) continue;
			} else {
				continue;
			}
			std::filesystem::remove_all(it->path(), entryEc);
			if (entryEc) ++result.failed;
			else ++result.removed;
		}
		if (ec) ++result.failed;
		return result;
	}
}
