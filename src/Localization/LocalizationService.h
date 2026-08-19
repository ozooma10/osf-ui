#pragma once

#include <map>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace OSFUI
{
	// Resolve structural translation addresses with authored English as the final fallback.
	class LocalizationService
	{
	public:
		using Catalog = std::unordered_map<std::string, std::string>;

		// Load flat <modId>_<locale>.json address-to-UTF-8 catalogs.
		void Load(std::filesystem::path a_dir, std::string a_locale);

		// Normalize and change the active locale without re-reading catalogs.
		bool SetLocale(std::string a_locale);
		[[nodiscard]] const std::string& Locale() const { return _locale; }

		// Resolve exact locale, base language, en catalog, then authored English.
		[[nodiscard]] std::string Resolve(std::string_view a_modId,
			std::string_view a_address,
			std::string_view a_authoredEnglish) const;

		// Return merged overrides without inventing defaults for bridge consumers.
		[[nodiscard]] nlohmann::json CatalogFor(std::string_view a_modId) const;

		// Re-read catalogs only when the dev-mode directory snapshot changes.
		bool ReloadIfChanged();

		[[nodiscard]] static std::string NormalizeLocale(std::string_view a_locale);
		// Read sLanguage from Starfield INIs, falling back to en.
		[[nodiscard]] static std::string DetectGameLocale(const std::filesystem::path& a_starfieldDir);

	private:
		using CatalogKey = std::pair<std::string, std::string>;  // mod, locale
		using FileSnapshot = std::map<std::filesystem::path, std::filesystem::file_time_type>;

		void LoadFiles();
		[[nodiscard]] FileSnapshot Scan() const;
		[[nodiscard]] std::vector<std::string> FallbackLocales() const;

		std::filesystem::path                     _dir;
		std::string                               _locale{ "en" };
		std::map<CatalogKey, Catalog> _catalogs;
		FileSnapshot                              _snapshot;
	};
}
