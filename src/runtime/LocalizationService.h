#pragma once

#include <map>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace OSFUI
{
	// One localization source for settings schemas, view manifests, built-in
	// chrome, and custom views. Authors write English; translation files
	// override structural addresses such as "settings.toggleKey.label".
	// Missing entries fall back to the authored English supplied by the caller.
	class LocalizationService
	{
	public:
		using Catalog = std::unordered_map<std::string, std::string>;

		// Loads every <modId>_<locale>.json under a_dir. Files are flat JSON
		// objects of structural address -> translated UTF-8 string.
		void Load(std::filesystem::path a_dir, std::string a_locale);

		// Changes the active locale without re-reading files. Returns true when
		// the effective locale changed. Locale names are normalized (de_DE ->
		// de-DE, English -> en, PortugueseBrazil -> pt-BR).
		bool SetLocale(std::string a_locale);
		[[nodiscard]] const std::string& Locale() const { return _locale; }

		// Resolve one address. Lookup order: exact locale, base language, en,
		// authored English. An en catalog file can therefore copy-edit an
		// authored source string.
		[[nodiscard]] std::string Resolve(std::string_view a_modId,
			std::string_view a_address,
			std::string_view a_authoredEnglish) const;

		// Effective merged overrides for bridge consumers. Invents no defaults:
		// a custom view supplies its inline English to osfui.t(address,
		// english).
		[[nodiscard]] nlohmann::json CatalogFor(std::string_view a_modId) const;

		// Re-read catalogs when their directory snapshot changes. Intended for
		// dev-mode polling; false means no visible filesystem change.
		bool ReloadIfChanged();

		[[nodiscard]] static std::string NormalizeLocale(std::string_view a_locale);
		// Reads sLanguage from Starfield.ini / StarfieldPrefs.ini under the
		// supplied My Games/Starfield directory. Returns "en" when unavailable.
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
