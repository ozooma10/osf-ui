// Host-side unit tests for SettingsStore (docs/mcm-design.md §8.3): the REAL
// src/runtime/SettingsStore.cpp + Json.cpp compiled against stubs/pch.h, run
// on the developer's desktop toolchain — the native mirror of the web
// devtools/harness. Assert-style; process exit code is the failure count.

#include "runtime/SettingsStore.h"

#include "core/Log.h"

namespace
{
	int  g_failures = 0;
	int  g_checks = 0;

#define CHECK(expr)                                                                     \
	do {                                                                                \
		++g_checks;                                                                     \
		if (!(expr)) {                                                                  \
			++g_failures;                                                               \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);        \
		}                                                                               \
	} while (0)

	void WriteFile(const std::filesystem::path& a_path, std::string_view a_text)
	{
		std::filesystem::create_directories(a_path.parent_path());
		std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
		out << a_text;
	}

	bool LoggedContaining(std::string_view a_level, std::string_view a_needle)
	{
		for (const auto& entry : REX::test::Entries()) {
			if (entry.starts_with(a_level) && entry.find(a_needle) != std::string::npos) {
				return true;
			}
		}
		return false;
	}

	struct Fired
	{
		std::string    mod;
		std::string    key;
		nlohmann::json value;
	};
}

// core/Log.h declarations (real impl lives in src/core/Log.cpp, which pulls
// game deps — stub it here instead).
namespace OSFUI::Log
{
	static bool g_devMode = true;

	void WarnOnce(std::once_flag& a_flag, std::string_view a_message)
	{
		std::call_once(a_flag, [&] { REX::test::Log("WARN", std::string(a_message)); });
	}

	bool DevMode() { return g_devMode; }
	void SetDevMode(bool a_enabled) { g_devMode = a_enabled; }
}

int main()
{
	using OSFUI::SettingsStore;
	namespace fs = std::filesystem;

	const auto root = fs::temp_directory_path() / "osfui-settings-store-tests";
	fs::remove_all(root);
	const auto schemaDir = root / "settings";
	const auto valuesDir = root / "values";

	// --- fixtures -----------------------------------------------------------
	WriteFile(schemaDir / "alpha.json", R"json({
		"id": "alpha", "title": "Alpha Mod",
		"groups": [ { "label": "General", "settings": [
			{ "key": "enabled", "type": "bool",   "default": true },
			{ "key": "scale",   "type": "float",  "default": 1.0, "min": 0.5, "max": 2.0 },
			{ "key": "mode",    "type": "enum",   "default": "compact", "options": ["compact", "full"] },
			{ "key": "name",    "type": "string", "default": "hi" },
			{ "key": "bind",    "type": "key",    "default": "F10" }
		] } ] })json");
	WriteFile(schemaDir / "beta.json", R"json({
		"id": "beta", "title": "Beta Mod",
		"groups": [ { "label": "G", "settings": [
			{ "key": "count", "type": "int", "default": 3, "min": 0, "max": 10 }
		] } ] })json");
	// Duplicate id in a later-sorted file: first-loaded must win.
	WriteFile(schemaDir / "zeta.json", R"json({
		"id": "beta", "title": "Impostor Beta",
		"groups": [ { "label": "G", "settings": [
			{ "key": "evil", "type": "bool", "default": true }
		] } ] })json");
	// Persisted values: clamped on load, unknown keys ignored.
	WriteFile(valuesDir / "alpha.json", R"json({ "scale": 9.0, "mode": "full", "junk": 5 })json");
	// Persisted values for a mod that registers at runtime only.
	WriteFile(valuesDir / "gamma.json", R"json({ "level": 7 })json");

	// --- RegisterSchema before LoadAll is rejected ---------------------------
	{
		SettingsStore fresh;
		CHECK(!fresh.RegisterSchema(nlohmann::json{ { "id", "early" } }, SettingsStore::Source::kNative));
	}

	// --- LoadAll: overlay, clamp, duplicate handling -------------------------
	SettingsStore store;
	store.LoadAll(schemaDir, valuesDir);
	const auto genAfterLoad = store.Generation();

	auto data = nlohmann::json::parse(store.DataJson());
	CHECK(data["mods"].size() == 2);  // alpha + beta once; zeta's duplicate dropped
	CHECK(LoggedContaining("WARN", "duplicate schema id 'beta'"));

	CHECK(store.GetValue("alpha", "enabled") && *store.GetValue("alpha", "enabled") == true);
	CHECK(store.GetValue("alpha", "scale") && store.GetValue("alpha", "scale")->get<double>() == 2.0);  // 9.0 clamped
	CHECK(store.GetValue("alpha", "mode") && *store.GetValue("alpha", "mode") == "full");               // persisted
	CHECK(store.GetValue("alpha", "junk") == nullptr);                                                  // never adopted
	CHECK(store.GetValue("beta", "evil") == nullptr);                                                   // impostor schema dropped
	CHECK(store.GetValue("nope", "x") == nullptr);

	CHECK(store.GetSettingType("alpha", "bind") == "key");
	CHECK(store.GetSettingType("alpha", "scale") == "float");
	CHECK(store.GetSettingType("alpha", "nope").empty());
	CHECK(store.GetSettingType("nope", "bind").empty());

	// --- multicast listeners --------------------------------------------------
	std::vector<Fired> heard1, heard2;
	store.AddChangeListener([&](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
		heard1.push_back({ std::string(a_mod), std::string(a_key), a_value });
	});
	store.AddChangeListener([&](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
		heard2.push_back({ std::string(a_mod), std::string(a_key), a_value });
	});

	CHECK(store.Set("alpha", "scale", "1.5"));
	CHECK(heard1.size() == 1 && heard1.back().mod == "alpha" && heard1.back().key == "scale" && heard1.back().value == 1.5);
	CHECK(heard2.size() == 1 && heard2.back().value == 1.5);

	// Persisted through the normal path.
	{
		auto saved = nlohmann::json::parse(std::ifstream(valuesDir / "alpha.json"), nullptr, false);
		CHECK(saved.is_object() && saved["scale"] == 1.5);
	}

	// --- validation still rejects (no notify on reject) -----------------------
	heard1.clear();
	CHECK(!store.Set("alpha", "scale", "\"big\""));     // wrong type
	CHECK(!store.Set("alpha", "mode", "\"neon\""));     // not an option
	CHECK(!store.Set("alpha", "ghost", "1"));           // unknown key
	CHECK(!store.Set("ghost", "scale", "1"));           // unknown mod
	CHECK(heard1.empty());

	// --- Reset: one key, then whole mod ---------------------------------------
	CHECK(store.Reset("alpha", "scale"));
	CHECK(store.GetValue("alpha", "scale")->get<double>() == 1.0);
	CHECK(store.Reset("alpha", ""));
	CHECK(*store.GetValue("alpha", "mode") == "compact");

	// --- NotifyMod replays current values --------------------------------------
	heard1.clear();
	store.NotifyMod("alpha");
	CHECK(heard1.size() == 5);  // one per alpha setting
	store.NotifyMod("ghost");   // unknown: no fire, no crash
	CHECK(heard1.size() == 5);

	// --- incremental RegisterSchema: new mod, persisted overlay, replay -------
	heard1.clear();
	auto gammaSchema = nlohmann::json::parse(R"json({
		"id": "gamma", "title": "Gamma (runtime)",
		"groups": [ { "label": "G", "settings": [
			{ "key": "level", "type": "int", "default": 1, "min": 0, "max": 10 },
			{ "key": "fancy", "type": "bool", "default": false }
		] } ] })json");
	CHECK(store.RegisterSchema(gammaSchema, SettingsStore::Source::kNative));
	CHECK(store.Generation() > genAfterLoad);
	CHECK(store.GetValue("gamma", "level")->get<std::int64_t>() == 7);  // pre-existing values file adopted
	CHECK(*store.GetValue("gamma", "fancy") == false);
	CHECK(heard1.size() == 2);  // per-mod replay fired for both values
	data = nlohmann::json::parse(store.DataJson());
	CHECK(data["mods"].size() == 3);

	// Rejected shapes.
	CHECK(!store.RegisterSchema(nlohmann::json::array(), SettingsStore::Source::kNative));
	CHECK(!store.RegisterSchema(nlohmann::json{ { "title", "No Id" } }, SettingsStore::Source::kNative));

	// Runtime-registered mods persist through the same per-mod file.
	CHECK(store.Set("gamma", "level", "9"));
	{
		auto saved = nlohmann::json::parse(std::ifstream(valuesDir / "gamma.json"), nullptr, false);
		CHECK(saved.is_object() && saved["level"] == 9);
	}

	// --- precedence: native replaces drop-in; drop-in never replaces ----------
	CHECK(store.Set("alpha", "scale", "0.75"));  // user value that must survive the tier upgrade
	auto alphaV2 = nlohmann::json::parse(R"json({
		"id": "alpha", "title": "Alpha Mod v2",
		"groups": [ { "label": "General", "settings": [
			{ "key": "scale",  "type": "float", "default": 1.0, "min": 0.5, "max": 2.0 },
			{ "key": "shiny",  "type": "bool",  "default": true }
		] } ] })json");
	const auto genBeforeReplace = store.Generation();
	CHECK(store.RegisterSchema(alphaV2, SettingsStore::Source::kNative));
	CHECK(LoggedContaining("WARN", "runtime registration replaces drop-in"));
	CHECK(store.Generation() > genBeforeReplace);
	data = nlohmann::json::parse(store.DataJson());
	CHECK(data["mods"].size() == 3);  // replaced, not duplicated
	CHECK(store.GetValue("alpha", "scale")->get<double>() == 0.75);  // persisted user value survived
	CHECK(*store.GetValue("alpha", "shiny") == true);                // new key gets default
	CHECK(store.GetValue("alpha", "enabled") == nullptr);            // removed key gone
	CHECK(store.GetSettingType("alpha", "bind").empty());

	// A drop-in may not displace the native registration.
	CHECK(!store.RegisterSchema(nlohmann::json{ { "id", "alpha" }, { "title", "Stale File" } }, SettingsStore::Source::kDropIn));
	data = nlohmann::json::parse(store.DataJson());
	for (const auto& mod : data["mods"]) {
		if (mod["id"] == "alpha") {
			CHECK(mod["title"] == "Alpha Mod v2");
		}
	}

	// Native re-registration (dev iteration) replaces its own earlier one.
	auto alphaV3 = alphaV2;
	alphaV3["title"] = "Alpha Mod v3";
	CHECK(store.RegisterSchema(alphaV3, SettingsStore::Source::kNative));
	data = nlohmann::json::parse(store.DataJson());
	CHECK(data["mods"].size() == 3);

	// --- RemoveMod: registry drops, values file kept ----------------------------
	CHECK(store.Set("beta", "count", "8"));
	const auto genBeforeRemove = store.Generation();
	CHECK(store.RemoveMod("beta"));
	CHECK(store.Generation() > genBeforeRemove);
	CHECK(!store.RemoveMod("beta"));
	CHECK(store.GetValue("beta", "count") == nullptr);
	CHECK(fs::exists(valuesDir / "beta.json"));  // uninstalled ≠ deleted (mcm-design.md §10)
	data = nlohmann::json::parse(store.DataJson());
	CHECK(data["mods"].size() == 2);

	// ---------------------------------------------------------------------------
	std::fprintf(stderr, "%d/%d checks passed\n", g_checks - g_failures, g_checks);
	fs::remove_all(root);
	return g_failures;
}
