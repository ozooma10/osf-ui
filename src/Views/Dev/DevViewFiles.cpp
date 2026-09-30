#include "Views/Dev/DevViewFiles.h"
#include "Core/Utf8Path.h"
#include "Views/ViewCache.h"

namespace OSFUI::DevViewFiles
{
	namespace
	{
		bool Fail(std::string& a_error, const std::filesystem::path& a_path, const std::error_code& a_ec)
		{
			a_error = Utf8Path(a_path.filename()) + ": " + a_ec.message();
			return false;
		}
	}  // namespace

	std::optional<std::uint64_t> Fingerprint(const std::filesystem::path& a_viewDir)
	{
		// Same walk the views cache uses; manifests are excluded at every depth because their discovery is restart-only.
		std::string error;
		const auto fingerprint = ViewCache::FingerprintTree(a_viewDir, {}, error, [](const std::filesystem::path& a_file) {
			return a_file.filename() == "manifest.json";
		});
		if (!fingerprint) return std::nullopt;
		return fingerprint->value;
	}

	bool ReplaceTree(const std::filesystem::path& a_source, const std::filesystem::path& a_destination, std::string& a_error)
	{
		a_error.clear();
		std::error_code ec;
		if (!std::filesystem::is_directory(a_source, ec) || ec) {
			a_error = ec ? ec.message() : "source is not a directory";
			return false;
		}
		std::filesystem::remove_all(a_destination, ec);
		if (ec) return Fail(a_error, a_destination, ec);
		std::filesystem::create_directories(a_destination, ec);
		if (ec) return Fail(a_error, a_destination, ec);
		std::filesystem::copy(a_source, a_destination, std::filesystem::copy_options::recursive, ec);
		if (ec) return Fail(a_error, a_source, ec);
		return true;
	}
}  // namespace OSFUI::DevViewFiles
