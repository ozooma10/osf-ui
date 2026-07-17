// Host-side coverage for the English-source localization catalog and locale
// fallback rules. Compiles the real service; no game/Windows dependency.

#include "runtime/LocalizationService.h"

#include "core/Log.h"

namespace
{
	int g_failures = 0;
	int g_checks = 0;

#define CHECK(expr)                                                              \
	do {                                                                           \
		++g_checks;                                                                \
		if (!(expr)) {                                                             \
			++g_failures;                                                          \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
		}                                                                          \
	} while (0)

	void WriteFile(const std::filesystem::path& a_path, std::string_view a_text)
	{
		std::filesystem::create_directories(a_path.parent_path());
		std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
		out << a_text;
	}
}

namespace OSFUI::Log
{
	void WarnOnce(std::once_flag& a_flag, std::string_view a_message)
	{
		std::call_once(a_flag, [&] { REX::test::Log("WARN", std::string(a_message)); });
	}
	bool DevMode() { return true; }
	void SetDevMode(bool) {}
}

int main()
{
	using OSFUI::LocalizationService;
	namespace fs = std::filesystem;
	const auto root = fs::temp_directory_path() / "osfui-localization-tests";
	fs::remove_all(root);
	const auto l10n = root / "l10n";

	WriteFile(l10n / "t.mod_en.json", R"({
		"settings.title": "English override",
		"settings.mode.label": "Mode"
	})");
	WriteFile(l10n / "t.mod_de.json", R"({
		"settings.title": "Deutscher Titel"
	})");
	WriteFile(l10n / "t.mod_de-DE.json", R"({
		"settings.mode.label": "Modus (DE)"
	})");

	LocalizationService service;
	service.Load(l10n, "de_DE");
	CHECK(service.Locale() == "de-DE");
	CHECK(service.Resolve("t.mod", "settings.title", "Authored title") == "Deutscher Titel");
	CHECK(service.Resolve("t.mod", "settings.mode.label", "Authored mode") == "Modus (DE)");
	CHECK(service.Resolve("t.mod", "settings.missing.label", "Inline English") == "Inline English");
	const auto merged = service.CatalogFor("t.mod");
	CHECK(merged["settings.title"] == "Deutscher Titel");
	CHECK(merged["settings.mode.label"] == "Modus (DE)");

	CHECK(LocalizationService::NormalizeLocale("PortugueseBrazil") == "pt-BR");
	CHECK(LocalizationService::NormalizeLocale("zh_hans") == "zh-Hans");
	CHECK(LocalizationService::NormalizeLocale("bad locale!") == "en");
	CHECK(service.SetLocale("fr-FR"));
	CHECK(!service.SetLocale("fr_FR"));
	CHECK(service.Resolve("t.mod", "settings.title", "Authored title") == "English override");

	const auto gameDir = root / "My Games" / "Starfield";
	WriteFile(gameDir / "Starfield.ini", "[General]\nsLanguage=German\n");
	CHECK(LocalizationService::DetectGameLocale(gameDir) == "de");

	WriteFile(l10n / "t.mod_fr.json", R"({"settings.title":"Titre français"})");
	fs::last_write_time(l10n / "t.mod_fr.json", fs::file_time_type::clock::now() + std::chrono::seconds(2));
	CHECK(service.ReloadIfChanged());
	CHECK(service.Resolve("t.mod", "settings.title", "Authored title") == "Titre français");
	CHECK(!service.ReloadIfChanged());

	std::printf("localization_service_tests: %d checks, %d failure(s)\n", g_checks, g_failures);
	fs::remove_all(root);
	return g_failures == 0 ? 0 : 1;
}
