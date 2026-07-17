#include "runtime/LocalizationService.h"

#include <cctype>
#include <fstream>

#include "core/Log.h"
#include "runtime/Ids.h"
#include "runtime/Json.h"

namespace OSFUI
{
	namespace
	{
		std::string Trim(std::string_view a_text)
		{
			while (!a_text.empty() && std::isspace(static_cast<unsigned char>(a_text.front()))) {
				a_text.remove_prefix(1);
			}
			while (!a_text.empty() && std::isspace(static_cast<unsigned char>(a_text.back()))) {
				a_text.remove_suffix(1);
			}
			return std::string(a_text);
		}

		std::string Lower(std::string_view a_text)
		{
			std::string out(a_text);
			std::ranges::transform(out, out.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			return out;
		}

		std::optional<std::string> ReadIniLanguage(const std::filesystem::path& a_path)
		{
			std::ifstream file(a_path);
			if (!file) {
				return std::nullopt;
			}
			std::string line;
			while (std::getline(file, line)) {
				const auto trimmed = Trim(line);
				if (trimmed.empty() || trimmed.starts_with(';') || trimmed.starts_with('#')) {
					continue;
				}
				const auto eq = trimmed.find('=');
				if (eq == std::string::npos) {
					continue;
				}
				if (Lower(Trim(std::string_view(trimmed).substr(0, eq))) == "slanguage") {
					return Trim(std::string_view(trimmed).substr(eq + 1));
				}
			}
			return std::nullopt;
		}
	}

	void LocalizationService::Load(std::filesystem::path a_dir, std::string a_locale)
	{
		_dir = std::move(a_dir);
		_locale = NormalizeLocale(a_locale);
		LoadFiles();
	}

	bool LocalizationService::SetLocale(std::string a_locale)
	{
		a_locale = NormalizeLocale(a_locale);
		if (a_locale == _locale) {
			return false;
		}
		_locale = std::move(a_locale);
		REX::INFO("Localization: active locale -> {}", _locale);
		return true;
	}

	std::string LocalizationService::Resolve(std::string_view a_modId,
		std::string_view a_address,
		std::string_view a_authoredEnglish) const
	{
		for (const auto& locale : FallbackLocales()) {
			const auto catalog = _catalogs.find({ std::string(a_modId), locale });
			if (catalog == _catalogs.end()) {
				continue;
			}
			if (const auto value = catalog->second.find(std::string(a_address)); value != catalog->second.end()) {
				return value->second;
			}
		}
		return std::string(a_authoredEnglish);
	}

	nlohmann::json LocalizationService::CatalogFor(std::string_view a_modId) const
	{
		// Merge broad -> specific so later catalogs replace earlier ones.
		auto locales = FallbackLocales();
		std::ranges::reverse(locales);
		nlohmann::json out = nlohmann::json::object();
		for (const auto& locale : locales) {
			const auto catalog = _catalogs.find({ std::string(a_modId), locale });
			if (catalog == _catalogs.end()) {
				continue;
			}
			for (const auto& [address, value] : catalog->second) {
				out[address] = value;
			}
		}
		return out;
	}

	bool LocalizationService::ReloadIfChanged()
	{
		const auto snapshot = Scan();
		if (snapshot == _snapshot) {
			return false;
		}
		LoadFiles();
		return true;
	}

	std::string LocalizationService::NormalizeLocale(std::string_view a_locale)
	{
		auto text = Trim(a_locale);
		std::ranges::replace(text, '_', '-');
		const auto lower = Lower(text);
		static const std::unordered_map<std::string, std::string> aliases{
			{ "", "en" }, { "auto", "en" }, { "english", "en" },
			{ "german", "de" }, { "french", "fr" }, { "italian", "it" },
			{ "spanish", "es" }, { "spanishmexico", "es-MX" },
			{ "japanese", "ja" }, { "polish", "pl" },
			{ "portuguesebrazil", "pt-BR" }, { "brazilianportuguese", "pt-BR" },
			{ "chinesesimplified", "zh-Hans" }, { "chinesetraditional", "zh-Hant" },
			{ "korean", "ko" }, { "russian", "ru" },
		};
		if (const auto it = aliases.find(lower); it != aliases.end()) {
			return it->second;
		}
		if (text.empty()) {
			return "en";
		}
		std::string out;
		std::size_t start = 0;
		std::size_t part = 0;
		while (start <= text.size()) {
			const auto end = text.find('-', start);
			auto segment = text.substr(start, end == std::string::npos ? text.size() - start : end - start);
			if (segment.empty() || !std::ranges::all_of(segment, [](unsigned char c) { return std::isalnum(c); })) {
				return "en";
			}
			if (part == 0) {
				segment = Lower(segment);
			} else if (segment.size() == 2 || segment.size() == 3) {
				std::ranges::transform(segment, segment.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
			} else if (segment.size() == 4) {
				segment = Lower(segment);
				segment.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(segment.front())));
			}
			if (!out.empty()) {
				out += '-';
			}
			out += segment;
			++part;
			if (end == std::string::npos) {
				break;
			}
			start = end + 1;
		}
		return out;
	}

	std::string LocalizationService::DetectGameLocale(const std::filesystem::path& a_starfieldDir)
	{
		if (a_starfieldDir.empty()) {
			return "en";
		}
		// Prefer the main INI; some installs/tools write the preference file.
		for (const auto* name : { "Starfield.ini", "StarfieldPrefs.ini", "StarfieldCustom.ini" }) {
			if (const auto language = ReadIniLanguage(a_starfieldDir / name)) {
				return NormalizeLocale(*language);
			}
		}
		return "en";
	}

	void LocalizationService::LoadFiles()
	{
		_catalogs.clear();
		_snapshot = Scan();
		for (const auto& [path, _] : _snapshot) {
			const auto stem = path.stem().string();
			const auto split = stem.rfind('_');
			if (split == std::string::npos || split == 0 || split + 1 >= stem.size()) {
				REX::WARN("Localization: {} must be named <modId>_<locale>.json", path.string());
				continue;
			}
			const auto mod = stem.substr(0, split);
			const auto locale = NormalizeLocale(std::string_view(stem).substr(split + 1));
			if (!Ids::IsAcceptedModId(mod)) {
				REX::WARN("Localization: {} has invalid mod id '{}'", path.string(), mod);
				continue;
			}
			const auto json = Json::ParseFile(path);
			if (!json || !json->is_object()) {
				REX::WARN("Localization: skipping invalid catalog {}", path.string());
				continue;
			}
			auto& catalog = _catalogs[{ mod, locale }];
			for (const auto& [address, value] : json->items()) {
				if (value.is_string() && !address.empty()) {
					catalog[address] = value.get<std::string>();
				} else {
					REX::WARN("Localization: {} entry '{}' is not a string — ignored", path.string(), address);
				}
			}
			REX::INFO("Localization: loaded {} {} ({} strings)", mod, locale, catalog.size());
		}
	}

	LocalizationService::FileSnapshot LocalizationService::Scan() const
	{
		FileSnapshot out;
		std::error_code ec;
		if (!std::filesystem::is_directory(_dir, ec)) {
			return out;
		}
		for (const auto& entry : std::filesystem::directory_iterator(_dir, ec)) {
			if (entry.is_regular_file(ec) && entry.path().extension() == ".json") {
				if (const auto mtime = entry.last_write_time(ec); !ec) {
					out.emplace(entry.path(), mtime);
				}
			}
		}
		return out;
	}

	std::vector<std::string> LocalizationService::FallbackLocales() const
	{
		std::vector<std::string> locales;
		const auto append = [&](std::string value) {
			if (!value.empty() && std::ranges::find(locales, value) == locales.end()) {
				locales.push_back(std::move(value));
			}
		};
		append(_locale);
		if (const auto dash = _locale.find('-'); dash != std::string::npos) {
			append(_locale.substr(0, dash));
		}
		append("en");
		return locales;
	}
}
