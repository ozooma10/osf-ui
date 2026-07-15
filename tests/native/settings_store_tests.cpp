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
			{ "key": "name",    "type": "string", "default": "hi", "maxLength": 4 },
			{ "key": "tint",    "type": "string", "widget": "color", "default": "#5aa9b8" },
			{ "key": "bind",    "type": "key",    "default": "F10" }
		] } ] })json");
	WriteFile(schemaDir / "beta.json", R"json({
		"id": "beta", "title": "Beta Mod",
		"groups": [ { "label": "G", "settings": [
			{ "key": "count", "type": "int", "default": 3, "min": 0, "max": 10 }
		] } ] })json");
	// A drop-in claiming another mod's id: the id MUST equal the filename stem,
	// so it registers as "zeta" (warned) and cannot hijack "beta".
	WriteFile(schemaDir / "zeta.json", R"json({
		"id": "beta", "title": "Impostor Beta",
		"groups": [ { "label": "G", "settings": [
			{ "key": "evil", "type": "bool", "default": true }
		] } ] })json");
	// A drop-in whose stem is unsafe as a filename/path segment: rejected.
	WriteFile(schemaDir / "bad id.json", R"json({
		"id": "bad id", "title": "Space Id",
		"groups": [ { "label": "G", "settings": [
			{ "key": "x", "type": "bool", "default": true }
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
	CHECK(data["mods"].size() == 3);  // alpha + beta + zeta (renamed); "bad id" rejected
	CHECK(LoggedContaining("WARN", "must equal the filename stem"));
	CHECK(LoggedContaining("WARN", "rejected schema id 'bad id'"));

	CHECK(store.GetValue("alpha", "enabled") && *store.GetValue("alpha", "enabled") == true);
	CHECK(store.GetValue("alpha", "scale") && store.GetValue("alpha", "scale")->get<double>() == 2.0);  // 9.0 clamped
	CHECK(store.GetValue("alpha", "mode") && *store.GetValue("alpha", "mode") == "full");               // persisted
	CHECK(store.GetValue("alpha", "junk") == nullptr);                                                  // never adopted
	CHECK(store.GetValue("beta", "evil") == nullptr);   // the impostor could not take beta's id...
	CHECK(store.GetValue("zeta", "evil") != nullptr);   // ...it registered under its own stem
	CHECK(store.GetValue("bad id", "x") == nullptr);
	CHECK(store.GetValue("nope", "x") == nullptr);

	// The document the web sees carries the EFFECTIVE id, not the impostor claim.
	for (const auto& mod : data["mods"]) {
		CHECK(mod["id"] == mod["schema"]["id"]);
	}

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

	// Persisted through the normal path (write-behind: flush forces the disk write).
	store.FlushPersistence();
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
	CHECK(!store.Set("alpha", "tint", "\"blue\""));     // colour widget: not a hex colour
	CHECK(heard1.empty());

	// Per-setting maxLength truncates; a colour widget accepts real hex.
	CHECK(store.Set("alpha", "name", "\"abcdefgh\""));
	CHECK(*store.GetValue("alpha", "name") == "abcd");
	CHECK(store.Set("alpha", "tint", "\"#112233\""));
	CHECK(*store.GetValue("alpha", "tint") == "#112233");

	// --- key type: "" is the unbound state, gated on allowUnbound -------------
	CHECK(!store.Set("alpha", "bind", "\"\""));  // no allowUnbound: blank refused
	CHECK(store.Set("alpha", "bind", "\"F9\""));
	CHECK(*store.GetValue("alpha", "bind") == "F9");
	{
		auto unboundSchema = nlohmann::json::parse(R"json({
			"id": "unbindy", "title": "Unbindy",
			"groups": [ { "label": "G", "settings": [
				{ "key": "hot", "type": "key", "default": "", "allowUnbound": true }
			] } ] })json");
		CHECK(store.RegisterSchema(unboundSchema, SettingsStore::Source::kNative));
		CHECK(*store.GetValue("unbindy", "hot") == "");     // empty default is legal
		CHECK(store.Set("unbindy", "hot", "\"F7\""));       // bind
		CHECK(store.Set("unbindy", "hot", "\"\""));         // deliberate unbind
		CHECK(*store.GetValue("unbindy", "hot") == "");
		// The unbound value enumerates as "" (HotkeyService/conflicts skip it
		// downstream via ResolveKeyName("") == invalid).
		for (const auto& ks : store.KeySettings()) {
			if (ks.modId == "unbindy") {
				CHECK(ks.name.empty());
			}
		}
		store.RemoveMod("unbindy");
	}

	// --- Reset: one key, then whole mod ---------------------------------------
	CHECK(store.Reset("alpha", "scale"));
	CHECK(store.GetValue("alpha", "scale")->get<double>() == 1.0);
	CHECK(store.Reset("alpha", ""));
	CHECK(*store.GetValue("alpha", "mode") == "compact");

	// --- NotifyMod replays current values --------------------------------------
	heard1.clear();
	store.NotifyMod("alpha");
	CHECK(heard1.size() == 6);  // one per alpha setting
	store.NotifyMod("ghost");   // unknown: no fire, no crash
	CHECK(heard1.size() == 6);

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
	CHECK(data["mods"].size() == 4);

	// Rejected shapes.
	CHECK(!store.RegisterSchema(nlohmann::json::array(), SettingsStore::Source::kNative));
	CHECK(!store.RegisterSchema(nlohmann::json{ { "title", "No Id" } }, SettingsStore::Source::kNative));

	// Rejected ids: traversal / unsafe charset / reserved framework namespaces.
	// An id names the values file and the asset folder, so it must never escape.
	for (const auto* bad : { "..\\..\\Starfield", "../evil", "a/b", "a\\b", "has space",
	                         ".hidden", "..", "menu", "settings", "ui", "hud", "views", "game", "runtime" }) {
		CHECK(!store.RegisterSchema(nlohmann::json{ { "id", bad }, { "title", "Evil" } }, SettingsStore::Source::kNative));
	}

	// Duplicate drop-in ids resolve first-wins (MO2's VFS is the arbiter of the
	// FILE; a second registration for the same id never displaces the first).
	CHECK(!store.RegisterSchema(nlohmann::json{ { "id", "zeta" }, { "title", "Zeta Again" } }, SettingsStore::Source::kDropIn));
	CHECK(LoggedContaining("WARN", "duplicate schema id 'zeta'"));

	// Runtime-registered mods persist through the same per-mod file.
	CHECK(store.Set("gamma", "level", "9"));
	store.FlushPersistence();
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
	CHECK(data["mods"].size() == 4);  // replaced, not duplicated
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
	CHECK(data["mods"].size() == 4);

	// --- RemoveMod: registry drops, values file kept ----------------------------
	CHECK(store.Set("beta", "count", "8"));
	const auto genBeforeRemove = store.Generation();
	CHECK(store.RemoveMod("beta"));
	CHECK(store.Generation() > genBeforeRemove);
	CHECK(!store.RemoveMod("beta"));
	CHECK(store.GetValue("beta", "count") == nullptr);
	CHECK(fs::exists(valuesDir / "beta.json"));  // uninstalled ≠ deleted (mcm-design.md §10)
	data = nlohmann::json::parse(store.DataJson());
	CHECK(data["mods"].size() == 3);

	// --- ValidateSchemaShape: the ABI's synchronous any-thread shape gate --------
	// Must reject exactly what registration would reject on shape/id grounds
	// (BridgeApi::RegisterSettingsSchema reports these synchronously, then
	// queues the main-thread merge).
	CHECK(SettingsStore::ValidateSchemaShape(nlohmann::json{ { "id", "ok-mod_1.x" } }));
	CHECK(!SettingsStore::ValidateSchemaShape(nlohmann::json::array()));               // not an object
	CHECK(!SettingsStore::ValidateSchemaShape(nlohmann::json{ { "title", "No Id" } }));
	for (const auto* bad : { "..\\..\\Starfield", "../evil", "a/b", "has space",
	                         ".hidden", "..", "menu", "settings", "ui" }) {
		CHECK(!SettingsStore::ValidateSchemaShape(nlohmann::json{ { "id", bad } }));
	}

	// --- GetSource: who owns an id (the ABI unregister gate) ---------------------
	CHECK(store.GetSource("alpha") == SettingsStore::Source::kNative);  // tier-upgraded above
	CHECK(store.GetSource("gamma") == SettingsStore::Source::kNative);
	CHECK(store.GetSource("zeta") == SettingsStore::Source::kDropIn);
	CHECK(!store.GetSource("beta").has_value());  // removed above
	CHECK(!store.GetSource("ghost").has_value());

	// --- write-behind debounce (mcm-design.md §8.1): coalesced, due after window --
	store.PumpPersistence(100.0);  // settle pending windows; store clock -> 100
	CHECK(store.Set("gamma", "level", "3"));
	CHECK(store.GetValue("gamma", "level")->get<std::int64_t>() == 3);  // committed in memory...
	{
		auto saved = nlohmann::json::parse(std::ifstream(valuesDir / "gamma.json"), nullptr, false);
		CHECK(saved["level"] == 9);  // ...but not on disk yet
	}
	store.PumpPersistence(100.0 + SettingsStore::kPersistDelaySeconds - 0.01);  // window still open
	{
		auto saved = nlohmann::json::parse(std::ifstream(valuesDir / "gamma.json"), nullptr, false);
		CHECK(saved["level"] == 9);
	}
	CHECK(store.Set("gamma", "level", "4"));  // joins the SAME window — no push-back
	store.PumpPersistence(100.0 + SettingsStore::kPersistDelaySeconds);  // due
	{
		auto saved = nlohmann::json::parse(std::ifstream(valuesDir / "gamma.json"), nullptr, false);
		CHECK(saved["level"] == 4);  // one write covered both steps
	}

	// --- sparse persistence: only ≠ default on disk; reset = key removal ----------
	CHECK(store.Set("gamma", "fancy", "true"));  // ≠ default (false)
	store.FlushPersistence();
	{
		auto saved = nlohmann::json::parse(std::ifstream(valuesDir / "gamma.json"), nullptr, false);
		CHECK((saved == nlohmann::json{ { "level", 4 }, { "fancy", true } }));  // defaults never written
	}
	CHECK(store.Reset("gamma", "level"));
	store.FlushPersistence();
	{
		auto saved = nlohmann::json::parse(std::ifstream(valuesDir / "gamma.json"), nullptr, false);
		CHECK((saved == nlohmann::json{ { "fancy", true } }));  // reset = key removal
	}

	// --- prune-to-default on load + teardown flush ---------------------------------
	{
		const auto schemaDir2 = root / "settings2";
		const auto valuesDir2 = root / "values2";
		fs::create_directories(schemaDir2);
		fs::create_directories(valuesDir2);
		WriteFile(schemaDir2 / "delta.json", R"json({
			"id": "delta", "title": "Delta",
			"groups": [ { "label": "G", "settings": [
				{ "key": "n", "type": "int",  "default": 3 },
				{ "key": "b", "type": "bool", "default": false }
			] } ] })json");
		// Legacy FULL file: "n" frozen at the (still-current) default, plus junk.
		WriteFile(valuesDir2 / "delta.json", R"json({ "n": 3, "b": true, "junk": 1 })json");

		{
			SettingsStore s2;
			s2.LoadAll(schemaDir2, valuesDir2);
			s2.PumpPersistence(SettingsStore::kPersistDelaySeconds);  // load opened a rewrite window
			{
				auto saved = nlohmann::json::parse(std::ifstream(valuesDir2 / "delta.json"), nullptr, false);
				CHECK((saved == nlohmann::json{ { "b", true } }));  // frozen default + junk pruned: "n" tracks upstream again
			}
			CHECK(s2.Set("delta", "n", "7"));
			// No pump, no flush: teardown must land it.
		}
		auto saved = nlohmann::json::parse(std::ifstream(valuesDir2 / "delta.json"), nullptr, false);
		CHECK((saved == nlohmann::json{ { "b", true }, { "n", 7 } }));  // ~SettingsStore flushed
	}

	// --- §11 renamed keys: per-setting `aliases` ----------------------------------
	{
		const auto sd = root / "settings-alias";
		const auto vd = root / "values-alias";
		WriteFile(sd / "ren.json", R"json({
			"id": "ren", "title": "Rename",
			"groups": [ { "settings": [
				{ "key": "opacity", "type": "int", "default": 50, "min": 0, "max": 100, "aliases": ["alpha", "hudAlpha"] },
				{ "key": "size",    "type": "int", "default": 10, "aliases": ["scale"] },
				{ "key": "plain",   "type": "int", "default": 1 }
			] } ] })json");
		// Old file uses the FIRST alias for opacity, a LATER alias for size,
		// and an alias whose value won't validate should never be adopted.
		WriteFile(vd / "ren.json", R"json({ "alpha": 80, "scale": 25 })json");

		SettingsStore s;
		s.LoadAll(sd, vd);
		CHECK(s.GetValue("ren", "opacity") && *s.GetValue("ren", "opacity") == 80);  // adopted from "alpha"
		CHECK(s.GetValue("ren", "size") && *s.GetValue("ren", "size") == 25);        // adopted from "scale"
		CHECK(s.GetValue("ren", "plain") && *s.GetValue("ren", "plain") == 1);       // untouched default
		CHECK(LoggedContaining("INFO", "adopted from alias 'alpha'"));

		// The rename rewrites under the NEW key; the old alias keys drop.
		s.PumpPersistence(SettingsStore::kPersistDelaySeconds);
		{
			auto saved = nlohmann::json::parse(std::ifstream(vd / "ren.json"), nullptr, false);
			CHECK((saved == nlohmann::json{ { "opacity", 80 }, { "size", 25 } }));  // no "alpha"/"scale" left
		}

		// The current key present wins over any alias; an alias that fails
		// validation (wrong type) falls through to default, not adopted.
		WriteFile(vd / "ren.json", R"json({ "opacity": 30, "alpha": 99, "scale": "nope" })json");
		SettingsStore s2;
		s2.LoadAll(sd, vd);
		CHECK(s2.GetValue("ren", "opacity") && *s2.GetValue("ren", "opacity") == 30);  // current key wins
		CHECK(s2.GetValue("ren", "size") && *s2.GetValue("ren", "size") == 10);        // "nope" invalid -> default
	}

	// --- §11 `$schemaVersion` meta key --------------------------------------------
	{
		const auto sd = root / "settings-ver";
		const auto vd = root / "values-ver";

		// A v0 (unversioned) mod NEVER gets a stamp — existing files untouched.
		WriteFile(sd / "unver.json", R"json({
			"id": "unver", "groups": [ { "settings": [
				{ "key": "n", "type": "int", "default": 1 }
			] } ] })json");
		// A versioned mod stamps $schemaVersion.
		WriteFile(sd / "ver.json", R"json({
			"id": "ver", "version": 3, "groups": [ { "settings": [
				{ "key": "n", "type": "int", "default": 1 }
			] } ] })json");
		WriteFile(vd / "ver.json", R"json({ "$schemaVersion": 2, "n": 5 })json");  // file from an older v2

		SettingsStore s;
		s.LoadAll(sd, vd);
		CHECK(LoggedContaining("INFO", "migrating v2 -> v3"));
		s.PumpPersistence(SettingsStore::kPersistDelaySeconds);
		{
			auto ver = nlohmann::json::parse(std::ifstream(vd / "ver.json"), nullptr, false);
			CHECK((ver == nlohmann::json{ { "$schemaVersion", 3 }, { "n", 5 } }));  // stamp advanced, value kept
		}

		// v0 mod: fresh install, all-default -> no file churn beyond none, and
		// crucially NO $schemaVersion key.
		SettingsStore su;
		su.LoadAll(sd, vd);
		su.FlushPersistence();
		{
			std::error_code ec;
			// unver never diverged from sparse-empty, so no file need exist;
			// if one does (defensive), it must not carry a version stamp.
			if (fs::exists(vd / "unver.json", ec)) {
				auto un = nlohmann::json::parse(std::ifstream(vd / "unver.json"), nullptr, false);
				CHECK(!un.contains("$schemaVersion"));
			}
		}

		// No perpetual re-dirty: a versioned file already at the current
		// version + sparse form must load CLEAN (no rewrite scheduled). Prove
		// it by loading, immediately flushing, and checking the byte content
		// is unchanged even though we never pumped a rewrite window.
		WriteFile(vd / "ver.json", R"json({"$schemaVersion":3,"n":5})json");
		SettingsStore sc;
		sc.LoadAll(sd, vd);
		// A clean load leaves the mod not-dirty; FlushPersistence is then a
		// no-op and the (compact, hand-written) file keeps its exact bytes.
		sc.FlushPersistence();
		{
			std::ifstream f(vd / "ver.json");
			std::string   contents((std::istreambuf_iterator<char>(f)), {});
			CHECK(contents == R"json({"$schemaVersion":3,"n":5})json");  // untouched: clean load
		}
	}

	// --- §12.1 ReloadDropInFile: dev schema hot-reload -----------------------------
	{
		const auto sd = root / "settings-hot";
		const auto vd = root / "values-hot";
		WriteFile(sd / "hot.json", R"json({
			"id": "hot", "title": "Hot v1",
			"groups": [ { "settings": [
				{ "key": "speed", "type": "int", "default": 5, "min": 0, "max": 10 }
			] } ] })json");

		SettingsStore s;
		s.LoadAll(sd, vd);
		CHECK(s.Set("hot", "speed", "8"));  // a live, unflushed (dirty) user value

		std::size_t registryFires = 0;
		s.AddRegistryListener([&] { ++registryFires; });

		// Reload with a retitled schema + an added setting + a key RENAME via
		// §11 aliases. The dirty value must survive: the reload flushes the
		// write-behind window first, then overlays from the file it just wrote
		// — and the alias carries it across the rename.
		WriteFile(sd / "hot.json", R"json({
			"id": "hot", "title": "Hot v2",
			"groups": [ { "settings": [
				{ "key": "velocity", "type": "int", "default": 5, "min": 0, "max": 10, "aliases": ["speed"] },
				{ "key": "brandNew", "type": "bool", "default": true }
			] } ] })json");
		CHECK(s.ReloadDropInFile(sd / "hot.json"));
		CHECK(registryFires == 1);
		CHECK(s.GetValue("hot", "velocity") && *s.GetValue("hot", "velocity") == 8);  // dirty value survived + renamed
		CHECK(s.GetValue("hot", "brandNew") && *s.GetValue("hot", "brandNew") == true);
		CHECK(s.GetValue("hot", "speed") == nullptr);  // the old key is gone
		{
			const auto data = s.Data();
			CHECK(data["mods"][0]["title"] == "Hot v2");
		}

		// Invalid JSON: refused, registered schema untouched.
		WriteFile(sd / "hot.json", "{ not json");
		CHECK(!s.ReloadDropInFile(sd / "hot.json"));
		CHECK(s.GetValue("hot", "velocity") != nullptr);

		// An unseen id registers as a fresh drop-in.
		WriteFile(sd / "newcomer.json", R"json({
			"id": "newcomer", "groups": [ { "settings": [
				{ "key": "x", "type": "int", "default": 0 }
			] } ] })json");
		CHECK(s.ReloadDropInFile(sd / "newcomer.json"));
		CHECK(s.GetValue("newcomer", "x") != nullptr);

		// A runtime (native) registration outranks the file: refused.
		CHECK(s.RegisterSchema(nlohmann::json::parse(R"json({
			"id": "owned", "title": "Native",
			"groups": [ { "settings": [ { "key": "k", "type": "int", "default": 1 } ] } ] })json"),
			SettingsStore::Source::kNative));
		WriteFile(sd / "owned.json", R"json({
			"id": "owned", "title": "File",
			"groups": [ { "settings": [ { "key": "k", "type": "int", "default": 2 } ] } ] })json");
		CHECK(!s.ReloadDropInFile(sd / "owned.json"));
		CHECK(s.GetValue("owned", "k") && *s.GetValue("owned", "k") == 1);
	}

	// ---------------------------------------------------------------------------
	std::fprintf(stderr, "%d/%d checks passed\n", g_checks - g_failures, g_checks);
	fs::remove_all(root);
	return g_failures;
}
