
#include "Settings/SettingsStore.h"

#include "Core/Log.h"
#include "check.h"

namespace
{

	[[nodiscard]] bool StrictDumpOk(const nlohmann::json& a_value)
	{
		try {
			(void)a_value.dump();
			return true;
		} catch (const std::exception&) {
			return false;
		}
	}

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

namespace OSFUI::Log
{
	static bool g_debugEnabled = true;

	void WarnOnce(std::once_flag& a_flag, std::string_view a_message)
	{
		std::call_once(a_flag, [&] { REX::test::Log("WARN", std::string(a_message)); });
	}

	bool DebugEnabled() { return g_debugEnabled; }
	void SetDebugLogging(bool a_enabled) { g_debugEnabled = a_enabled; }
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
	WriteFile(schemaDir / "t.alpha.json", R"json({
		"id": "t.alpha", "title": "Alpha Mod",
		"groups": [ { "label": "General", "settings": [
			{ "key": "enabled", "type": "bool",   "default": true },
			{ "key": "scale",   "type": "float",  "default": 1.0, "min": 0.5, "max": 2.0 },
			{ "key": "mode",    "type": "enum",   "default": "compact", "options": ["compact", "full"] },
			{ "key": "name",    "type": "string", "default": "hi", "maxLength": 4 },
			{ "key": "tint",    "type": "string", "widget": "color", "default": "#5aa9b8" },
			{ "key": "bind",    "type": "key",    "default": "F10",
			  "onPress": { "script": "MyMod:Hotkeys", "function": "OnHotkey" } }
		] } ] })json");
	WriteFile(schemaDir / "t.beta.json", R"json({
		"id": "t.beta", "title": "Beta Mod",
		"groups": [ { "label": "G", "settings": [
			{ "key": "count", "type": "int", "default": 3, "min": 0, "max": 10 }
		] } ] })json");
	WriteFile(schemaDir / "t.zeta.json", R"json({
		"id": "t.beta", "title": "Impostor Beta",
		"groups": [ { "label": "G", "settings": [
			{ "key": "evil", "type": "bool", "default": true }
		] } ] })json");
	// Opaque ids may contain spaces and need no dot.
	WriteFile(schemaDir / "bad id.json", R"json({
		"id": "bad id", "title": "Space Id",
		"groups": [ { "label": "G", "settings": [
			{ "key": "x", "type": "bool", "default": true }
		] } ] })json");
	WriteFile(schemaDir / "plainmod.json", R"json({
		"id": "plainmod", "title": "Dotless",
		"groups": [ { "label": "G", "settings": [
			{ "key": "x", "type": "bool", "default": true }
		] } ] })json");
	WriteFile(schemaDir / "OSFUI.json", R"json({ "id": "OSFUI", "title": "Impostor" })json");
	// Persisted values: clamped on load, unknown keys ignored.
	WriteFile(valuesDir / "t.alpha.json", R"json({ "scale": 9.0, "mode": "full", "junk": 5 })json");
	// Persisted values for a mod whose drop-in appears after startup.
	WriteFile(valuesDir / "t.gamma.json", R"json({ "level": 7 })json");

	// Deprecated native registration is rejected until the store has a values
	// directory, then retains its historical precedence while old mods migrate.
	{
		SettingsStore fresh;
		CHECK(!fresh.RegisterSchema(nlohmann::json{ { "id", "t.early" } }, SettingsStore::Source::kNative));
	}

	// --- LoadAll: overlay, clamp, duplicate handling -------------------------
	SettingsStore store;
	store.LoadAll(schemaDir, valuesDir);
	const auto genAfterLoad = store.Generation();

	auto data = nlohmann::json::parse(store.DataJson());
	CHECK(data["mods"].size() == 5);  // alpha + beta + zeta + two opaque ids; OSFUI alias rejected
	CHECK(LoggedContaining("WARN", "must equal the filename stem"));
	// A reserved alias is skipped with an ERROR naming the file.
	CHECK(LoggedContaining("ERROR", "OSFUI.json"));

	CHECK(store.GetValue("t.alpha", "enabled") && *store.GetValue("t.alpha", "enabled") == true);
	CHECK(store.GetValue("t.alpha", "scale") && store.GetValue("t.alpha", "scale")->get<double>() == 2.0);  // 9.0 clamped
	CHECK(store.GetValue("t.alpha", "mode") && *store.GetValue("t.alpha", "mode") == "full");               // persisted
	CHECK(store.GetValue("t.alpha", "junk") == nullptr);                                                  // unknown key: preserved on disk, never served
	CHECK(store.GetValue("t.beta", "evil") == nullptr);   // the impostor could not take beta's id...
	CHECK(store.GetValue("t.zeta", "evil") != nullptr);   // ...it registered under its own stem
	CHECK(store.GetValue("bad id", "x") != nullptr);
	CHECK(store.GetValue("plainmod", "x") != nullptr);
	CHECK(store.GetValue("nope", "x") == nullptr);

	// Runtime-only schema registration remains functional, adopts the same
	// persisted values, replaces a drop-in, and cannot be displaced by one.
	{
		const auto nativeSchemaDir = root / "native-settings";
		const auto nativeValuesDir = root / "native-values";
		WriteFile(nativeSchemaDir / "t.native.json", R"json({
			"id": "t.native", "title": "Drop-in",
			"groups": [{ "settings": [{ "key": "level", "type": "int", "default": 1 }] }]
		})json");
		WriteFile(nativeValuesDir / "t.native.json", R"json({ "level": 7 })json");
		SettingsStore nativeStore;
		nativeStore.LoadAll(nativeSchemaDir, nativeValuesDir);
		CHECK(nativeStore.GetSource("t.native") == SettingsStore::Source::kDropIn);
		CHECK(nativeStore.RegisterSchema(nlohmann::json::parse(R"json({
			"id": "t.native", "title": "Runtime",
			"groups": [{ "settings": [
				{ "key": "level", "type": "int", "default": 2 },
				{ "key": "enabled", "type": "bool", "default": true }
			] }]
		})json"), SettingsStore::Source::kNative));
		CHECK(nativeStore.GetSource("t.native") == SettingsStore::Source::kNative);
		CHECK(nativeStore.GetValue("t.native", "level") && *nativeStore.GetValue("t.native", "level") == 7);
		CHECK(nativeStore.GetValue("t.native", "enabled") && *nativeStore.GetValue("t.native", "enabled") == true);
		CHECK(!nativeStore.ReloadDropInFile(nativeSchemaDir / "t.native.json"));
		CHECK(nativeStore.GetSource("t.native") == SettingsStore::Source::kNative);
		CHECK(nativeStore.RemoveMod("t.native"));
		CHECK(!nativeStore.GetSource("t.native").has_value());
	}

	CHECK(SettingsStore::ValidateSchemaShape(nlohmann::json{ { "id", "opaque mod!" } }));
	CHECK(!SettingsStore::ValidateSchemaShape(nlohmann::json::array()));
	CHECK(!SettingsStore::ValidateSchemaShape(nlohmann::json{ { "title", "No Id" } }));
	CHECK(!SettingsStore::ValidateSchemaShape(nlohmann::json{ { "id", "osfui" } }));

	store.SetTextResolver([](std::string_view mod, std::string_view address, std::string_view english) {
		if (mod == "t.alpha" && address == "settings.title") return std::string("Alpha übersetzt");
		if (mod == "t.alpha" && address == "groups.0.label") return std::string("Allgemein");
		if (mod == "t.alpha" && address == "settings.mode.label") return std::string("Modus");
		return std::string(english);
	});
	data = store.Data();
	const auto alphaLocalized = std::ranges::find_if(data["mods"], [](const auto& mod) { return mod["id"] == "t.alpha"; });
	CHECK(alphaLocalized != data["mods"].end());
	CHECK((*alphaLocalized)["title"] == "Alpha übersetzt");
	CHECK((*alphaLocalized)["schema"]["groups"][0]["label"] == "Allgemein");
	store.SetTextResolver({});

	// The document the web sees carries the EFFECTIVE id, not the impostor claim.
	for (const auto& mod : data["mods"]) {
		CHECK(mod["id"] == mod["schema"]["id"]);
	}

	CHECK(store.GetSettingType("t.alpha", "bind") == "key");
	CHECK(store.GetSettingType("t.alpha", "scale") == "float");
	CHECK(store.GetSettingType("t.alpha", "nope").empty());
	CHECK(store.GetSettingType("nope", "bind").empty());
	{
		const auto target = store.GetHotkeyTarget("t.alpha", "bind");
		CHECK(target && target->script == "MyMod:Hotkeys" && target->function == "OnHotkey");
		CHECK(store.HotkeyTargetIssues().empty());
	}

	// --- multicast listeners --------------------------------------------------
	std::vector<Fired> heard1, heard2;
	store.AddChangeListener([&](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
		heard1.push_back({ std::string(a_mod), std::string(a_key), a_value });
	});
	store.AddChangeListener([&](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
		heard2.push_back({ std::string(a_mod), std::string(a_key), a_value });
	});

	CHECK(store.Set("t.alpha", "scale", "1.5"));
	CHECK(heard1.size() == 1 && heard1.back().mod == "t.alpha" && heard1.back().key == "scale" && heard1.back().value == 1.5);
	CHECK(heard2.size() == 1 && heard2.back().value == 1.5);

	// Persisted through the normal path (write-behind: flush forces the disk write).
	store.FlushPersistence();
	{
		auto saved = nlohmann::json::parse(std::ifstream(valuesDir / "t.alpha.json"), nullptr, false);
		CHECK(saved.is_object() && saved["scale"] == 1.5);
	}

	// --- validation still rejects (no notify on reject) -----------------------
	heard1.clear();
	CHECK(!store.Set("t.alpha", "scale", "\"big\""));     // wrong type
	CHECK(!store.Set("t.alpha", "mode", "\"neon\""));     // not an option
	CHECK(!store.Set("t.alpha", "ghost", "1"));           // unknown key
	CHECK(!store.Set("ghost", "scale", "1"));           // unknown mod
	CHECK(!store.Set("t.alpha", "tint", "\"blue\""));     // colour widget: not a hex colour
	CHECK(heard1.empty());

	// Per-setting maxLength truncates; a colour widget accepts real hex.
	CHECK(store.Set("t.alpha", "name", "\"abcdefgh\""));
	CHECK(*store.GetValue("t.alpha", "name") == "abcd");
	CHECK(store.Set("t.alpha", "tint", "\"#112233\""));
	CHECK(*store.GetValue("t.alpha", "tint") == "#112233");

	// --- key type: "" is the unbound state, gated on allowUnbound -------------
	CHECK(!store.Set("t.alpha", "bind", "\"\""));  // no allowUnbound: blank refused
	CHECK(store.Set("t.alpha", "bind", "\"F9\""));
	CHECK(*store.GetValue("t.alpha", "bind") == "F9");
	{
		WriteFile(schemaDir / "t.unbindy.json", R"json({
			"id": "t.unbindy", "title": "Unbindy",
			"groups": [ { "label": "G", "settings": [
				{ "key": "hot", "type": "key", "default": "", "allowUnbound": true }
			] } ] })json");
		CHECK(store.ReloadDropInFile(schemaDir / "t.unbindy.json"));
		CHECK(*store.GetValue("t.unbindy", "hot") == "");     // empty default is legal
		CHECK(store.Set("t.unbindy", "hot", "\"F7\""));       // bind
		CHECK(store.Set("t.unbindy", "hot", "\"\""));         // deliberate unbind
		CHECK(*store.GetValue("t.unbindy", "hot") == "");
		for (const auto& ks : store.KeySettings()) {
			if (ks.modId == "t.unbindy") {
				CHECK(ks.name.empty());
			}
		}
		store.RemoveMod("t.unbindy");
	}

	// --- declarative hotkey targets are validated schema metadata ------------
	{
		auto targetSchema = nlohmann::json::parse(R"json({
			"id": "t.targets", "groups": [ { "settings": [
				{ "key": "good", "type": "key", "default": "F6",
				  "onPress": { "script": "Target_Lib", "function": "Fire" } },
				{ "key": "shape", "type": "key", "default": "F7", "onPress": "bad" },
				{ "key": "missing", "type": "key", "default": "F8",
				  "onPress": { "script": "Target_Lib" } },
				{ "key": "extra", "type": "key", "default": "F5",
				  "onPress": { "script": "Target_Lib", "function": "Fire", "args": [] } },
				{ "key": "wrongType", "type": "bool", "default": true,
				  "onPress": { "script": "Target_Lib", "function": "Fire" } }
			] } ]
		})json");
		targetSchema["groups"][0]["settings"].push_back({
			{ "key", "tooLong" }, { "type", "key" }, { "default", "F9" },
			{ "onPress", { { "script", std::string(129, 'A') }, { "function", "Fire" } } }
		});
		WriteFile(schemaDir / "t.targets.json", targetSchema.dump());
		CHECK(store.ReloadDropInFile(schemaDir / "t.targets.json"));
		const auto target = store.GetHotkeyTarget("t.targets", "good");
		CHECK(target && target->script == "Target_Lib" && target->function == "Fire");
		CHECK(!store.GetHotkeyTarget("t.targets", "shape"));
		CHECK(!store.GetHotkeyTarget("t.targets", "missing"));
		CHECK(!store.GetHotkeyTarget("t.targets", "extra"));
		CHECK(!store.GetHotkeyTarget("t.targets", "wrongType"));
		CHECK(!store.GetHotkeyTarget("t.targets", "tooLong"));
		CHECK(store.HotkeyTargetIssues().size() == 5);
		CHECK(store.Set("t.targets", "good", "\"F10\""));
		CHECK(!store.Set("t.targets", "onPress", R"({"script":"Evil","function":"Run"})"));
		CHECK(store.GetHotkeyTarget("t.targets", "good") == target);  // values cannot alter schema metadata

		targetSchema["groups"][0]["settings"] = nlohmann::json::array({
			{
				{ "key", "good" }, { "type", "key" }, { "default", "F6" },
				{ "onPress", { { "script", "Target_Lib2" }, { "function", "Fire2" } } }
			}
		});
		WriteFile(schemaDir / "t.targets.json", targetSchema.dump());
		CHECK(store.ReloadDropInFile(schemaDir / "t.targets.json"));
		CHECK(store.HotkeyTargetIssues().empty());
		const auto replacement = store.GetHotkeyTarget("t.targets", "good");
		CHECK(replacement && replacement->script == "Target_Lib2" && replacement->function == "Fire2");
		CHECK(store.RemoveMod("t.targets"));
		CHECK(!store.GetHotkeyTarget("t.targets", "good"));
	}

	// --- Reset: one key, then whole mod ---------------------------------------
	CHECK(store.Reset("t.alpha", "scale"));
	CHECK(store.GetValue("t.alpha", "scale")->get<double>() == 1.0);
	CHECK(store.Reset("t.alpha", ""));
	CHECK(*store.GetValue("t.alpha", "mode") == "compact");

	// --- NotifyMod replays current values --------------------------------------
	heard1.clear();
	store.NotifyMod("t.alpha");
	CHECK(heard1.size() == 6);  // one per alpha setting
	store.NotifyMod("ghost");   // unknown: no fire, no crash
	CHECK(heard1.size() == 6);

	// --- new drop-in after startup: persisted overlay and value replay --------
	heard1.clear();
	WriteFile(schemaDir / "t.gamma.json", R"json({
		"id": "t.gamma", "title": "Gamma",
		"groups": [ { "label": "G", "settings": [
			{ "key": "level", "type": "int", "default": 1, "min": 0, "max": 10 },
			{ "key": "fancy", "type": "bool", "default": false }
		] } ] })json");
	CHECK(store.ReloadDropInFile(schemaDir / "t.gamma.json"));
	CHECK(store.Generation() > genAfterLoad);
	CHECK(store.GetValue("t.gamma", "level")->get<std::int64_t>() == 7);  // pre-existing values file adopted
	CHECK(*store.GetValue("t.gamma", "fancy") == false);
	CHECK(heard1.size() == 2);  // per-mod replay fired for both values
	data = nlohmann::json::parse(store.DataJson());
	CHECK(data["mods"].size() == 6);

	// Newly discovered drop-ins persist through the same per-mod file.
	CHECK(store.Set("t.gamma", "level", "9"));
	store.FlushPersistence();
	{
		auto saved = nlohmann::json::parse(std::ifstream(valuesDir / "t.gamma.json"), nullptr, false);
		CHECK(saved.is_object() && saved["level"] == 9);
	}

	// --- file hot reload replaces schema while preserving user values ---------
	CHECK(store.Set("t.alpha", "scale", "0.75"));
	auto alphaV2 = nlohmann::json::parse(R"json({
		"id": "t.alpha", "title": "Alpha Mod v2",
		"groups": [ { "label": "General", "settings": [
			{ "key": "scale",  "type": "float", "default": 1.0, "min": 0.5, "max": 2.0 },
			{ "key": "shiny",  "type": "bool",  "default": true }
		] } ] })json");
	const auto genBeforeReplace = store.Generation();
	WriteFile(schemaDir / "t.alpha.json", alphaV2.dump());
	CHECK(store.ReloadDropInFile(schemaDir / "t.alpha.json"));
	CHECK(store.Generation() > genBeforeReplace);
	data = nlohmann::json::parse(store.DataJson());
	CHECK(data["mods"].size() == 6);  // replaced, not duplicated
	CHECK(store.GetValue("t.alpha", "scale")->get<double>() == 0.75);  // persisted user value survived
	CHECK(*store.GetValue("t.alpha", "shiny") == true);                // new key gets default
	CHECK(store.GetValue("t.alpha", "enabled") == nullptr);            // removed key gone
	CHECK(store.GetSettingType("t.alpha", "bind").empty());

	// A later edit replaces the same file-backed schema again.
	auto alphaV3 = alphaV2;
	alphaV3["title"] = "Alpha Mod v3";
	WriteFile(schemaDir / "t.alpha.json", alphaV3.dump());
	CHECK(store.ReloadDropInFile(schemaDir / "t.alpha.json"));
	data = nlohmann::json::parse(store.DataJson());
	CHECK(data["mods"].size() == 6);

	// --- RemoveMod: registry drops, values file kept ----------------------------
	CHECK(store.Set("t.beta", "count", "8"));
	const auto genBeforeRemove = store.Generation();
	CHECK(store.RemoveMod("t.beta"));
	CHECK(store.Generation() > genBeforeRemove);
	CHECK(!store.RemoveMod("t.beta"));
	CHECK(store.GetValue("t.beta", "count") == nullptr);
	CHECK(fs::exists(valuesDir / "t.beta.json"));  // uninstalled does not mean deleted
	data = nlohmann::json::parse(store.DataJson());
	CHECK(data["mods"].size() == 5);

	// --- write-behind debounce: coalesced, due after window -----------------
	store.PumpPersistence(100.0);  // settle pending windows; store clock -> 100
	CHECK(store.Set("t.gamma", "level", "3"));
	CHECK(store.GetValue("t.gamma", "level")->get<std::int64_t>() == 3);  // committed in memory...
	{
		auto saved = nlohmann::json::parse(std::ifstream(valuesDir / "t.gamma.json"), nullptr, false);
		CHECK(saved["level"] == 9);  // ...but not on disk yet
	}
	store.PumpPersistence(100.0 + SettingsStore::kPersistDelaySeconds - 0.01);  // window still open
	{
		auto saved = nlohmann::json::parse(std::ifstream(valuesDir / "t.gamma.json"), nullptr, false);
		CHECK(saved["level"] == 9);
	}
	CHECK(store.Set("t.gamma", "level", "4"));  // joins the SAME window — no push-back
	store.PumpPersistence(100.0 + SettingsStore::kPersistDelaySeconds);  // due
	{
		auto saved = nlohmann::json::parse(std::ifstream(valuesDir / "t.gamma.json"), nullptr, false);
		CHECK(saved["level"] == 4);  // one write covered both steps
	}

	// --- failed persistence stays dirty and retries after a bounded backoff -------
	{
		const auto sd = root / "settings-retry";
		const auto blockedValues = root / "values-retry";
		WriteFile(sd / "t.retry.json", R"json({
			"id": "t.retry", "groups": [ { "settings": [
				{ "key": "n", "type": "int", "default": 1 }
			] } ] })json");
		WriteFile(blockedValues, "not a directory");

		SettingsStore retry;
		int persisted = 0;
		retry.AddPersistListener([&](std::string_view) { ++persisted; });
		retry.LoadAll(sd, blockedValues);
		CHECK(retry.Set("t.retry", "n", "2"));
		retry.PumpPersistence(SettingsStore::kPersistDelaySeconds);
		CHECK(persisted == 0);
		CHECK(!fs::exists(blockedValues / "t.retry.json"));

		std::error_code ec;
		fs::remove(blockedValues, ec);
		CHECK(!ec);
		fs::create_directories(blockedValues, ec);
		CHECK(!ec);
		retry.PumpPersistence(2.0 * SettingsStore::kPersistDelaySeconds);
		CHECK(persisted == 1);
		auto saved = nlohmann::json::parse(
			std::ifstream(blockedValues / "t.retry.json"), nullptr, false);
		CHECK(saved["n"] == 2);
	}

	// --- failed prerequisite flush cannot discard a dirty schema -----------------
	{
		const auto sd = root / "settings-replace-guard";
		const auto blockedValues = root / "values-replace-guard";
		const auto schemaPath = sd / "t.replace-guard.json";
		WriteFile(schemaPath, R"json({
			"id": "t.replace-guard", "title": "Replace Guard v1",
			"groups": [ { "settings": [
				{ "key": "speed", "type": "int", "default": 5, "min": 0, "max": 10 }
			] } ] })json");
		WriteFile(blockedValues, "not a directory");

		SettingsStore guarded;
		std::size_t registryFires = 0;
		std::size_t persisted = 0;
		guarded.AddRegistryListener([&] { ++registryFires; });
		guarded.AddPersistListener([&](std::string_view) { ++persisted; });
		guarded.LoadAll(sd, blockedValues);
		CHECK(guarded.Set("t.replace-guard", "speed", "8"));
		const auto generationBefore = guarded.Generation();

		WriteFile(schemaPath, R"json({
			"id": "t.replace-guard", "title": "Replace Guard v2",
			"groups": [ { "settings": [
				{ "key": "velocity", "type": "int", "default": 5, "min": 0, "max": 10,
				  "aliases": ["speed"] },
				{ "key": "added", "type": "bool", "default": true }
			] } ] })json");
		CHECK(!guarded.ReloadDropInFile(schemaPath));
		CHECK(guarded.Generation() == generationBefore);
		CHECK(registryFires == 0);
		CHECK(persisted == 0);
		CHECK(guarded.GetValue("t.replace-guard", "speed") &&
		      *guarded.GetValue("t.replace-guard", "speed") == 8);
		CHECK(guarded.GetValue("t.replace-guard", "velocity") == nullptr);
		CHECK(guarded.GetValue("t.replace-guard", "added") == nullptr);
		CHECK(guarded.DataView()["mods"][0]["title"] == "Replace Guard v1");

		std::error_code ec;
		fs::remove(blockedValues, ec);
		CHECK(!ec);
		fs::create_directories(blockedValues, ec);
		CHECK(!ec);
		guarded.PumpPersistence(SettingsStore::kPersistDelaySeconds);
		CHECK(persisted == 1);
		{
			auto saved = nlohmann::json::parse(
				std::ifstream(blockedValues / "t.replace-guard.json"), nullptr, false);
			CHECK(saved["speed"] == 8);
		}
		CHECK(guarded.ReloadDropInFile(schemaPath));
		CHECK(registryFires == 1);
		CHECK(guarded.GetValue("t.replace-guard", "speed") == nullptr);
		CHECK(guarded.GetValue("t.replace-guard", "velocity") &&
		      *guarded.GetValue("t.replace-guard", "velocity") == 8);
		CHECK(guarded.GetValue("t.replace-guard", "added") &&
		      *guarded.GetValue("t.replace-guard", "added") == true);
	}

	// --- failed prerequisite flush cannot discard a dirty removed mod ------------
	{
		const auto sd = root / "settings-remove-guard";
		const auto blockedValues = root / "values-remove-guard";
		WriteFile(sd / "t.remove-guard.json", R"json({
			"id": "t.remove-guard", "groups": [ { "settings": [
				{ "key": "n", "type": "int", "default": 1 }
			] } ] })json");
		WriteFile(blockedValues, "not a directory");

		SettingsStore guarded;
		std::size_t registryFires = 0;
		std::size_t persisted = 0;
		guarded.AddRegistryListener([&] { ++registryFires; });
		guarded.AddPersistListener([&](std::string_view) { ++persisted; });
		guarded.LoadAll(sd, blockedValues);
		CHECK(guarded.Set("t.remove-guard", "n", "2"));
		const auto generationBefore = guarded.Generation();

		CHECK(!guarded.RemoveMod("t.remove-guard"));
		CHECK(guarded.Generation() == generationBefore);
		CHECK(registryFires == 0);
		CHECK(persisted == 0);
		CHECK(guarded.GetValue("t.remove-guard", "n") &&
		      *guarded.GetValue("t.remove-guard", "n") == 2);

		std::error_code ec;
		fs::remove(blockedValues, ec);
		CHECK(!ec);
		fs::create_directories(blockedValues, ec);
		CHECK(!ec);
		guarded.PumpPersistence(SettingsStore::kPersistDelaySeconds);
		CHECK(persisted == 1);
		{
			auto saved = nlohmann::json::parse(
				std::ifstream(blockedValues / "t.remove-guard.json"), nullptr, false);
			CHECK(saved["n"] == 2);
		}
		CHECK(guarded.RemoveMod("t.remove-guard"));
		CHECK(guarded.GetValue("t.remove-guard", "n") == nullptr);
		CHECK(guarded.Generation() > generationBefore);
		CHECK(registryFires == 1);
	}

	// --- sparse persistence: only ≠ default on disk; reset = key removal ----------
	CHECK(store.Set("t.gamma", "fancy", "true"));  // ≠ default (false)
	store.FlushPersistence();
	{
		auto saved = nlohmann::json::parse(std::ifstream(valuesDir / "t.gamma.json"), nullptr, false);
		CHECK((saved == nlohmann::json{ { "level", 4 }, { "fancy", true }, { "$formatVersion", 1 } }));  // defaults never written
	}
	CHECK(store.Reset("t.gamma", "level"));
	store.FlushPersistence();
	{
		auto saved = nlohmann::json::parse(std::ifstream(valuesDir / "t.gamma.json"), nullptr, false);
		CHECK((saved == nlohmann::json{ { "fancy", true }, { "$formatVersion", 1 } }));  // reset = key removal
	}

	// --- prune-to-default on load + teardown flush ---------------------------------
	{
		const auto schemaDir2 = root / "settings2";
		const auto valuesDir2 = root / "values2";
		fs::create_directories(schemaDir2);
		fs::create_directories(valuesDir2);
		WriteFile(schemaDir2 / "t.delta.json", R"json({
			"id": "t.delta", "title": "Delta",
			"groups": [ { "label": "G", "settings": [
				{ "key": "n", "type": "int",  "default": 3 },
				{ "key": "b", "type": "bool", "default": false }
			] } ] })json");
		WriteFile(valuesDir2 / "t.delta.json", R"json({ "n": 3, "b": true, "junk": 1 })json");

		{
			SettingsStore s2;
			s2.LoadAll(schemaDir2, valuesDir2);
			s2.PumpPersistence(SettingsStore::kPersistDelaySeconds);  // load opened a rewrite window
			{
				auto saved = nlohmann::json::parse(std::ifstream(valuesDir2 / "t.delta.json"), nullptr, false);
				CHECK((saved == nlohmann::json{ { "b", true }, { "junk", 1 }, { "$formatVersion", 1 } }));  // frozen default pruned; unknown key preserved
			}
			CHECK(s2.Set("t.delta", "n", "7"));
			// No pump, no flush: teardown must land it.
		}
		auto saved = nlohmann::json::parse(std::ifstream(valuesDir2 / "t.delta.json"), nullptr, false);
		CHECK((saved == nlohmann::json{ { "b", true }, { "junk", 1 }, { "n", 7 }, { "$formatVersion", 1 } }));  // ~SettingsStore flushed, opaque kept
	}

	// --- §11 renamed keys: per-setting `aliases` ----------------------------------
	{
		const auto sd = root / "settings-alias";
		const auto vd = root / "values-alias";
		WriteFile(sd / "t.ren.json", R"json({
			"id": "t.ren", "title": "Rename",
			"groups": [ { "settings": [
				{ "key": "opacity", "type": "int", "default": 50, "min": 0, "max": 100, "aliases": ["alpha", "hudAlpha"] },
				{ "key": "size",    "type": "int", "default": 10, "aliases": ["scale"] },
				{ "key": "plain",   "type": "int", "default": 1 }
			] } ] })json");
		WriteFile(vd / "t.ren.json", R"json({ "alpha": 80, "scale": 25 })json");

		SettingsStore s;
		s.LoadAll(sd, vd);
		CHECK(s.GetValue("t.ren", "opacity") && *s.GetValue("t.ren", "opacity") == 80);  // adopted from "alpha"
		CHECK(s.GetValue("t.ren", "size") && *s.GetValue("t.ren", "size") == 25);        // adopted from "scale"
		CHECK(s.GetValue("t.ren", "plain") && *s.GetValue("t.ren", "plain") == 1);       // untouched default
		CHECK(LoggedContaining("INFO", "adopted from alias 'alpha'"));

		// The rename rewrites under the NEW key; the old alias keys drop.
		s.PumpPersistence(SettingsStore::kPersistDelaySeconds);
		{
			auto saved = nlohmann::json::parse(std::ifstream(vd / "t.ren.json"), nullptr, false);
			CHECK((saved == nlohmann::json{ { "opacity", 80 }, { "size", 25 }, { "$formatVersion", 1 } }));  // no "alpha"/"scale" left
		}

		WriteFile(vd / "t.ren.json", R"json({ "opacity": 30, "alpha": 99, "scale": "nope" })json");
		SettingsStore s2;
		s2.LoadAll(sd, vd);
		CHECK(s2.GetValue("t.ren", "opacity") && *s2.GetValue("t.ren", "opacity") == 30);  // current key wins
		CHECK(s2.GetValue("t.ren", "size") && *s2.GetValue("t.ren", "size") == 10);        // "nope" invalid -> default
	}

	// --- §11 `$schemaVersion` meta key --------------------------------------------
	{
		const auto sd = root / "settings-ver";
		const auto vd = root / "values-ver";

		// A v0 (unversioned) mod NEVER gets a stamp — existing files untouched.
		WriteFile(sd / "t.unver.json", R"json({
			"id": "t.unver", "groups": [ { "settings": [
				{ "key": "n", "type": "int", "default": 1 }
			] } ] })json");
		// A versioned mod stamps $schemaVersion.
		WriteFile(sd / "t.ver.json", R"json({
			"id": "t.ver", "version": 3, "groups": [ { "settings": [
				{ "key": "n", "type": "int", "default": 1 }
			] } ] })json");
		WriteFile(vd / "t.ver.json", R"json({ "$schemaVersion": 2, "n": 5 })json");  // file from an older v2

		SettingsStore s;
		s.LoadAll(sd, vd);
		CHECK(LoggedContaining("INFO", "migrating v2 -> v3"));
		s.PumpPersistence(SettingsStore::kPersistDelaySeconds);
		{
			auto ver = nlohmann::json::parse(std::ifstream(vd / "t.ver.json"), nullptr, false);
			CHECK((ver == nlohmann::json{ { "$schemaVersion", 3 }, { "n", 5 }, { "$formatVersion", 1 } }));  // stamp advanced, value kept
		}

		SettingsStore su;
		su.LoadAll(sd, vd);
		su.FlushPersistence();
		{
			std::error_code ec;
			if (fs::exists(vd / "t.unver.json", ec)) {
				auto un = nlohmann::json::parse(std::ifstream(vd / "t.unver.json"), nullptr, false);
				CHECK(!un.contains("$schemaVersion"));
			}
		}

		WriteFile(vd / "t.ver.json", R"json({"$schemaVersion":3,"n":5})json");
		SettingsStore sc;
		sc.LoadAll(sd, vd);
		sc.FlushPersistence();
		{
			std::ifstream f(vd / "t.ver.json");
			std::string   contents((std::istreambuf_iterator<char>(f)), {});
			CHECK(contents == R"json({"$schemaVersion":3,"n":5})json");  // untouched: clean load
		}
	}

	// --- §12.1 ReloadDropInFile: dev schema hot-reload -----------------------------
	{
		const auto sd = root / "settings-hot";
		const auto vd = root / "values-hot";
		WriteFile(sd / "t.hot.json", R"json({
			"id": "t.hot", "title": "Hot v1",
			"groups": [ { "settings": [
				{ "key": "speed", "type": "int", "default": 5, "min": 0, "max": 10 },
				{ "key": "hot", "type": "key", "default": "F6",
				  "onPress": { "script": "HotV1", "function": "Fire" } }
			] } ] })json");

		SettingsStore s;
		s.LoadAll(sd, vd);
		CHECK(s.Set("t.hot", "speed", "8"));  // a live, unflushed (dirty) user value
		const auto hotV1 = s.GetHotkeyTarget("t.hot", "hot");
		CHECK(hotV1 && hotV1->script == "HotV1");

		std::size_t registryFires = 0;
		s.AddRegistryListener([&] { ++registryFires; });

		WriteFile(sd / "t.hot.json", R"json({
			"id": "t.hot", "title": "Hot v2",
			"groups": [ { "settings": [
				{ "key": "velocity", "type": "int", "default": 5, "min": 0, "max": 10, "aliases": ["speed"] },
				{ "key": "brandNew", "type": "bool", "default": true },
				{ "key": "hot", "type": "key", "default": "F6",
				  "onPress": { "script": "HotV2", "function": "FireAgain" } }
			] } ] })json");
		CHECK(s.ReloadDropInFile(sd / "t.hot.json"));
		CHECK(registryFires == 1);
		CHECK(s.GetValue("t.hot", "velocity") && *s.GetValue("t.hot", "velocity") == 8);  // dirty value survived + renamed
		CHECK(s.GetValue("t.hot", "brandNew") && *s.GetValue("t.hot", "brandNew") == true);
		CHECK(s.GetValue("t.hot", "speed") == nullptr);  // the old key is gone
		const auto hotV2 = s.GetHotkeyTarget("t.hot", "hot");
		CHECK(hotV2 && hotV2->script == "HotV2");
		{
			const auto data = s.Data();
			CHECK(data["mods"][0]["title"] == "Hot v2");
		}

		// Invalid JSON: refused, registered schema untouched.
		WriteFile(sd / "t.hot.json", "{ not json");
		CHECK(!s.ReloadDropInFile(sd / "t.hot.json"));
		CHECK(s.GetValue("t.hot", "velocity") != nullptr);

		// An unseen id registers as a fresh drop-in.
		WriteFile(sd / "t.newcomer.json", R"json({
			"id": "t.newcomer", "groups": [ { "settings": [
				{ "key": "x", "type": "int", "default": 0 }
			] } ] })json");
		CHECK(s.ReloadDropInFile(sd / "t.newcomer.json"));
		CHECK(s.GetValue("t.newcomer", "x") != nullptr);

	}

	// --- item 2: flags type ---------------------------------------------------------
	{
		const auto sd = root / "settings-flags";
		const auto vd = root / "values-flags";
		WriteFile(sd / "t.flaggy.json", R"json({
			"id": "t.flaggy", "title": "Flaggy",
			"groups": [ { "settings": [
				{ "key": "widgets", "type": "flags", "options": ["clock", "compass", "o2"], "default": ["clock"] }
			] } ] })json");
		WriteFile(vd / "t.flaggy.json", R"json({ "widgets": ["o2", "zzz", "clock", "o2"] })json");

		SettingsStore s;
		s.LoadAll(sd, vd);
		CHECK(s.GetValue("t.flaggy", "widgets") && *s.GetValue("t.flaggy", "widgets") == nlohmann::json::array({ "clock", "o2" }));
		CHECK(s.GetSettingType("t.flaggy", "widgets") == "flags");
		CHECK(s.Set("t.flaggy", "widgets", R"(["compass", "clock"])"));  // canonicalized to declared order
		CHECK(*s.GetValue("t.flaggy", "widgets") == nlohmann::json::array({ "clock", "compass" }));
		CHECK(s.Set("t.flaggy", "widgets", "[]"));  // empty selection is legal
		CHECK(s.GetValue("t.flaggy", "widgets")->empty());
		CHECK(!s.Set("t.flaggy", "widgets", "\"clock\""));  // not an array: rejected
		CHECK(!s.Set("t.flaggy", "widgets", "3"));
	}

	// --- item 2: forward-compat preservation ------------------------------------------
	{
		const auto sd = root / "settings-fwd";
		const auto vd = root / "values-fwd";
		WriteFile(sd / "t.future.json", R"json({
			"id": "t.future", "title": "Future",
			"groups": [ { "settings": [
				{ "key": "vec", "type": "vector3", "default": [0,0,0], "aliases": ["oldvec"] },
				{ "key": "n",   "type": "int", "default": 1 }
			] } ] })json");
		WriteFile(vd / "t.future.json", R"json({ "vec": [1,2,3], "oldvec": [9,9,9], "mystery": {"a":1}, "n": 5 })json");

		SettingsStore s;
		s.LoadAll(sd, vd);
		// Unknown type: the schema default is served; the saved value withheld.
		CHECK(s.GetValue("t.future", "vec") && *s.GetValue("t.future", "vec") == nlohmann::json::array({ 0, 0, 0 }));
		CHECK(s.GetValue("t.future", "n") && *s.GetValue("t.future", "n") == 5);
		CHECK(s.GetValue("t.future", "mystery") == nullptr);  // never served
		// Writing an unknown-typed setting is refused (read-only until upgrade).
		CHECK(!s.Set("t.future", "vec", "[4,5,6]"));
		s.FlushPersistence();
		{
			std::ifstream f(vd / "t.future.json");
			std::string   contents((std::istreambuf_iterator<char>(f)), {});
			CHECK(contents == R"json({ "vec": [1,2,3], "oldvec": [9,9,9], "mystery": {"a":1}, "n": 5 })json");
		}
		CHECK(s.Set("t.future", "n", "9"));
		s.FlushPersistence();
		{
			auto saved = nlohmann::json::parse(std::ifstream(vd / "t.future.json"), nullptr, false);
			CHECK((saved == nlohmann::json{ { "vec", { 1, 2, 3 } }, { "oldvec", { 9, 9, 9 } },
			                                { "mystery", { { "a", 1 } } }, { "n", 9 }, { "$formatVersion", 1 } }));
		}
		// The web document serves the default too, and never the opaque bag.
		{
			const auto data = s.Data();
			for (const auto& mod : data["mods"]) {
				if (mod["id"] == "t.future") {
					CHECK(mod["values"]["vec"] == nlohmann::json::array({ 0, 0, 0 }));
					CHECK(!mod["values"].contains("mystery"));
					CHECK(!mod["values"].contains("oldvec"));
				}
			}
		}
	}

	// --- item 2: advisory targetVersion -------------------------------------------------
	{
		const auto sd = root / "settings-target";
		const auto vd = root / "values-target";
		WriteFile(sd / "t.future2.json", R"json({
			"id": "t.future2", "title": "Future", "targetVersion": "99.0.0",
			"groups": [ { "settings": [ { "key": "y", "type": "bool", "default": false } ] } ] })json");
		WriteFile(vd / "t.future2.json", R"json({ "y": true })json");
		// Malformed: ignored with a warning; Data() omits the field.
		WriteFile(sd / "t.badtarget.json", R"json({
			"id": "t.badtarget", "targetVersion": "soon",
			"groups": [ { "settings": [ { "key": "x", "type": "bool", "default": true } ] } ] })json");

		SettingsStore s;
		s.LoadAll(sd, vd);
		CHECK(LoggedContaining("WARN", "targets OSF UI 99.0.0"));
		CHECK(LoggedContaining("WARN", "targetVersion 'soon'"));
		// NOT a gate: the newer-targeted schema is fully live.
		CHECK(s.GetValue("t.future2", "y") && *s.GetValue("t.future2", "y") == true);
		CHECK(s.Set("t.future2", "y", "false"));
		CHECK(s.GetSettingType("t.future2", "y") == "bool");
		{
			const auto data = s.Data();
			bool found = false;
			for (const auto& mod : data["mods"]) {
				if (mod["id"] == "t.future2") {
					found = true;
					CHECK(mod["targetVersion"] == "99.0.0");
				} else {
					CHECK(!mod.contains("targetVersion"));
				}
			}
			CHECK(found);
		}
	}

	// --- item 5: SetWithResult refusal codes -----------------------------------------
	{
		const auto sd = root / "settings-codes";
		const auto vd = root / "values-codes";
		WriteFile(sd / "t.coded.json", R"json({
			"id": "t.coded",
			"groups": [ { "settings": [
				{ "key": "n",   "type": "int", "min": 0, "max": 10, "default": 5 },
				{ "key": "vec", "type": "vector3", "default": [0,0,0] }
			] } ] })json");

		SettingsStore s;
		s.LoadAll(sd, vd);
		CHECK(s.SetWithResult("t.coded", "n", "7").ok);
		CHECK(s.SetWithResult("t.coded", "n", "7").code.empty());
		CHECK(s.SetWithResult("nope.mod", "n", "7").code == "unknown-setting");
		CHECK(s.SetWithResult("t.coded", "nope", "7").code == "unknown-setting");
		CHECK(s.SetWithResult("t.coded", "n", "\"str\"").code == "invalid-value");
		CHECK(s.SetWithResult("t.coded", "n", "not json").code == "invalid-value");
		CHECK(s.SetWithResult("t.coded", "vec", "[1,2,3]").code == "read-only");   // unknown type
		// Clamp is SUCCESS (the ack carries the post-clamp value, not a code).
		CHECK(s.SetWithResult("t.coded", "n", "99").ok);
		CHECK(*s.GetValue("t.coded", "n") == 10);
		CHECK(s.DataView()["mods"][0]["values"]["n"] == 10);
		CHECK(s.SetValueWithResult("t.coded", "n", nlohmann::json(4)).ok);
		CHECK(*s.GetValue("t.coded", "n") == 4);
		CHECK(s.DataView()["mods"][0]["values"]["n"] == 4);
	}

	// --- item 8: values-file $formatVersion stamp ---------------------------------
	{
		const auto sd = root / "settings-fmt";
		const auto vd = root / "values-fmt";
		WriteFile(sd / "t.fmt.json", R"json({
			"id": "t.fmt",
			"groups": [ { "settings": [ { "key": "n", "type": "int", "default": 1 } ] } ] })json");

		// An already-stamped sparse file loads CLEAN (no rewrite churn).
		WriteFile(vd / "t.fmt.json", R"json({"$formatVersion":1,"n":5})json");
		{
			SettingsStore s;
			s.LoadAll(sd, vd);
			s.FlushPersistence();
			std::ifstream f(vd / "t.fmt.json");
			std::string   contents((std::istreambuf_iterator<char>(f)), {});
			CHECK(contents == R"json({"$formatVersion":1,"n":5})json");
		}
		WriteFile(vd / "t.fmt.json", R"json({ "$formatVersion": 7, "$futureMeta": "x", "n": 5 })json");
		{
			SettingsStore s;
			s.LoadAll(sd, vd);
			CHECK(LoggedContaining("INFO", "declares format v7"));
			CHECK(s.Set("t.fmt", "n", "6"));
			s.FlushPersistence();
			auto saved = nlohmann::json::parse(std::ifstream(vd / "t.fmt.json"), nullptr, false);
			CHECK(saved["$formatVersion"] == 7);
			CHECK(saved["$futureMeta"] == "x");
			CHECK(saved["n"] == 6);
		}
	}

	{
		const auto sd = root / "settings-keymig";
		const auto vd = root / "values-keymig";
		WriteFile(sd / "t.mig.json", R"json({
			"id": "t.mig", "groups": [ { "settings": [
				{ "key": "hot", "type": "key", "default": "F8" },
				{ "key": "alt", "type": "key", "default": "", "allowUnbound": true },
				{ "key": "renamed", "type": "key", "default": "F7", "aliases": ["oldname"] },
				{ "key": "n", "type": "int", "default": 3 }
			] } ] })json");

		const auto qwertz = [](const std::string& a_name) -> std::string {
			if (a_name == "Z") return "Y";
			if (a_name == "Semicolon") return "Grave";
			return a_name;
		};

		WriteFile(vd / "t.mig.json", R"json({ "hot": "Z", "alt": "", "oldname": "Semicolon", "n": 5 })json");
		{
			SettingsStore s;
			s.SetLegacyKeyMigrator(qwertz);
			s.LoadAll(sd, vd);
			CHECK(s.GetValue("t.mig", "hot") && *s.GetValue("t.mig", "hot") == "Y");
			CHECK(s.GetValue("t.mig", "renamed") && *s.GetValue("t.mig", "renamed") == "Grave");
			CHECK(s.GetValue("t.mig", "alt") && *s.GetValue("t.mig", "alt") == "");
			CHECK(LoggedContaining("INFO", "re-anchored to 'Y'"));
			s.PumpPersistence(SettingsStore::kPersistDelaySeconds);
			auto saved = nlohmann::json::parse(std::ifstream(vd / "t.mig.json"), nullptr, false);
			CHECK(saved["$formatVersion"] == 2);
			CHECK(saved["hot"] == "Y");
			CHECK(saved["renamed"] == "Grave");  // adopted under the NEW key
			CHECK(saved["n"] == 5);
		}
		{
			SettingsStore s;
			s.SetLegacyKeyMigrator([](const std::string& a_name) -> std::string {
				return a_name == "Y" ? std::string("Q") : a_name;
			});
			s.LoadAll(sd, vd);
			CHECK(s.GetValue("t.mig", "hot") && *s.GetValue("t.mig", "hot") == "Y");
		}
		WriteFile(vd / "t.mig.json", R"json({ "n": 5 })json");
		{
			SettingsStore s;
			s.SetLegacyKeyMigrator(qwertz);
			s.LoadAll(sd, vd);
			s.PumpPersistence(SettingsStore::kPersistDelaySeconds);
			auto saved = nlohmann::json::parse(std::ifstream(vd / "t.mig.json"), nullptr, false);
			CHECK(saved["$formatVersion"] == 2);
			CHECK(saved["n"] == 5);
		}
		WriteFile(vd / "t.mig.json", R"json({ "hot": "Z" })json");
		{
			SettingsStore s;
			s.LoadAll(sd, vd);
			CHECK(s.GetValue("t.mig", "hot") && *s.GetValue("t.mig", "hot") == "Z");
			CHECK(s.Set("t.mig", "n", "9"));
			s.FlushPersistence();
			auto saved = nlohmann::json::parse(std::ifstream(vd / "t.mig.json"), nullptr, false);
			CHECK(saved["$formatVersion"] == 1);
			CHECK(saved["hot"] == "Z");
		}
		WriteFile(vd / "t.mig.json", R"json({ "hot": "F6" })json");
		{
			SettingsStore s;
			s.SetLegacyKeyMigrator([](const std::string& a_name) { return a_name; });
			s.LoadAll(sd, vd);
			s.PumpPersistence(SettingsStore::kPersistDelaySeconds);
			auto saved = nlohmann::json::parse(std::ifstream(vd / "t.mig.json"), nullptr, false);
			CHECK((saved == nlohmann::json{ { "hot", "F6" }, { "$formatVersion", 2 } }));
		}
		{
			const auto vdFresh = root / "values-keymig-fresh";
			SettingsStore s;
			s.SetLegacyKeyMigrator(qwertz);
			s.LoadAll(sd, vdFresh);
			s.PumpPersistence(SettingsStore::kPersistDelaySeconds);
			CHECK(!fs::exists(vdFresh / "t.mig.json"));
			CHECK(s.Set("t.mig", "hot", "\"K\""));
			s.FlushPersistence();
			auto saved = nlohmann::json::parse(std::ifstream(vdFresh / "t.mig.json"), nullptr, false);
			CHECK(saved["$formatVersion"] == 2);
			CHECK(saved["hot"] == "K");
		}
	}

	{
		SettingsStore s;
		CHECK(!s.DataView().contains("keyboard"));
		s.SetKeyboardLabels("de-DE", { { "W", "W" }, { "Semicolon", "Ö" }, { "LShift", "Umsch" } });
		{
			const auto& data = s.DataView();
			CHECK(data.contains("keyboard"));
			CHECK(data["keyboard"]["layout"] == "de-DE");
			CHECK(data["keyboard"]["labels"]["Semicolon"] == "Ö");
			CHECK(data["keyboard"]["labels"]["LShift"] == "Umsch");
		}
		// Replacing the map invalidates the cached document (layout switch).
		s.SetKeyboardLabels("en-US", { { "Semicolon", ";" } });
		CHECK(s.DataView()["keyboard"]["layout"] == "en-US");
		CHECK(s.DataView()["keyboard"]["labels"]["Semicolon"] == ";");
		// Clearing removes the field entirely.
		s.SetKeyboardLabels("", {});
		CHECK(!s.DataView().contains("keyboard"));
	}

	// --- item 11: ConflictsForSetting (the settings.changed annotation) ---------------
	{
		const auto sd = root / "settings-cfs";
		const auto vd = root / "values-cfs";
		WriteFile(sd / "t.keya.json", R"json({
			"id": "t.keya", "title": "Key A",
			"groups": [ { "settings": [ { "key": "hot", "type": "key", "default": "F7" } ] } ] })json");
		WriteFile(sd / "t.keyb.json", R"json({
			"id": "t.keyb", "title": "Key B",
			"groups": [ { "settings": [ { "key": "also", "type": "key", "default": "F7" } ] } ] })json");

		SettingsStore s;
		// No resolver: no conflict grouping at all (mirrors Data()).
		s.LoadAll(sd, vd);
		CHECK(s.ConflictsForSetting("t.keya", "hot").empty());
		s.SetKeyNameResolver([](std::string_view a_name) -> std::uint32_t {
			if (a_name == "F7") return 0x76;
			if (a_name == "F8") return 0x77;
			return 0;
		});
		// Both sit on F7: each names the other.
		{
			const auto c = s.ConflictsForSetting("t.keya", "hot");
			CHECK(c.size() == 1);
			CHECK(!c.empty() && c[0]["mod"] == "t.keyb" && c[0]["key"] == "also" && c[0]["title"] == "Key B");
		}
		// Rebind away: the recomputed list is empty for BOTH sides.
		CHECK(s.Set("t.keya", "hot", "\"F8\""));
		CHECK(s.ConflictsForSetting("t.keya", "hot").empty());
		CHECK(s.ConflictsForSetting("t.keyb", "also").empty());
		// Non-key / unknown settings answer [] rather than erroring.
		CHECK(s.ConflictsForSetting("t.keya", "nope").empty());
	}

	// --- Load-error surfacing: skipped schemas + corrupt values ----------------
	{
		const auto sd = root / "loaderr" / "settings";
		const auto vd = root / "loaderr" / "values";
		WriteFile(sd / "t.good.json", R"json({ "id": "t.good",
			"groups": [ { "settings": [ { "key": "on", "type": "bool", "default": true } ] } ] })json");
		WriteFile(sd / "t.broken.json", R"json({ "id": "t.broken", )json");  // torn/unparseable
		WriteFile(sd / "bad%name.json", "{}");                               // unsafe filename stem
		WriteFile(vd / "t.good.json", R"json({ "on": fa)json");              // corrupt values

		SettingsStore s;
		s.LoadAll(sd, vd);

		// Only the good mod registered, on defaults (corrupt values never served).
		CHECK(s.DataView()["mods"].size() == 1);
		CHECK(s.GetValue("t.good", "on") && *s.GetValue("t.good", "on") == true);
		// §14.2 quarantine: the corrupt file is set aside, never silently dropped.
		CHECK(!fs::exists(vd / "t.good.json"));
		CHECK(fs::exists(vd / "t.good.json.bad"));

		CHECK(s.LoadErrors().size() == 3);
		const auto& data = s.DataView();
		const auto errs = data.contains("loadErrors") ? data["loadErrors"] : nlohmann::json::array();
		CHECK(errs.size() == 3);
		const auto findKind = [&](const char* a_kind) {
			for (const auto& e : errs) {
				if (e["kind"] == a_kind) return e;
			}
			return nlohmann::json{};
		};
		const auto parseErr = findKind("schema-parse");
		CHECK(!parseErr.is_null() && parseErr["file"] == "t.broken.json");
		CHECK(!parseErr.is_null() && parseErr["message"].get<std::string>().find("parse error") != std::string::npos);
		CHECK(!parseErr.is_null() && !parseErr.contains("mod"));
		const auto nameErr = findKind("schema-name");
		CHECK(!nameErr.is_null() && nameErr["file"] == "bad%name.json");
		const auto valuesErr = findKind("values-parse");
		CHECK(!valuesErr.is_null() && valuesErr["file"] == "t.good.json" && valuesErr["mod"] == "t.good");

		// A fixed file clears its issue; repeated failures replace rather than stack.
		WriteFile(sd / "t.broken.json", R"json({ "id": "t.broken",
			"groups": [ { "settings": [ { "key": "x", "type": "int", "default": 1 } ] } ] })json");
		const auto generationBeforeRecovery = s.Generation();
		CHECK(s.ReloadDropInFile(sd / "t.broken.json"));
		CHECK(s.Generation() > generationBeforeRecovery);
		CHECK(s.LoadErrors().size() == 2);
		CHECK(s.DataView()["mods"].size() == 2);
		int registryPings = 0;
		s.AddRegistryListener([&] { ++registryPings; });
		WriteFile(sd / "t.broken.json", R"json({ "id": )json");
		const auto generationBeforeFailure = s.Generation();
		CHECK(!s.ReloadDropInFile(sd / "t.broken.json"));
		CHECK(s.Generation() > generationBeforeFailure);
		const auto recordedErrorGeneration = s.Generation();
		CHECK(!s.ReloadDropInFile(sd / "t.broken.json"));
		CHECK(s.Generation() == recordedErrorGeneration);  // identical error is not a new state
		CHECK(s.LoadErrors().size() == 3);
		CHECK(registryPings >= 1);
		CHECK(s.DataView()["mods"].size() == 2);  // the last good parse stays registered

		CHECK(s.RemoveMod("t.good"));
		CHECK(s.LoadErrors().size() == 2);
	}

	{
		OSFUI::SettingsStore s;
		const auto sd = root / "ce-schemas";
		WriteFile(sd / "t.enum.json", R"json({ "id": "t.enum",
			"groups": [ { "settings": [
				{ "key": "mode", "type": "enum", "options": ["fast", "balanced"], "default": "balanced" },
				{ "key": "flag", "type": "bool", "default": true } ] } ] })json");
		s.LoadAll(sd, root / "ce-values");
		CHECK(s.CanonicalEnumValue("t.enum", "mode", "FAST") == "fast");
		CHECK(s.CanonicalEnumValue("t.enum", "mode", "Fast") == "fast");
		CHECK(s.CanonicalEnumValue("t.enum", "mode", "fast") == "fast");
		CHECK(!s.CanonicalEnumValue("t.enum", "mode", "bogus").has_value());
		CHECK(!s.CanonicalEnumValue("t.enum", "flag", "true").has_value());  // not enum-typed
		CHECK(!s.CanonicalEnumValue("t.nope", "mode", "fast").has_value());  // unknown mod
	}

	{
		OSFUI::SettingsStore s;
		const auto sd = root / "u8-schemas";
		WriteFile(sd / "t.utf8.json", R"json({ "id": "t.utf8",
			"groups": [ { "settings": [
				{ "key": "name", "type": "string", "maxLength": 8, "default": "" },
				{ "key": "long", "type": "string", "default": "" },
				{ "key": "emoji", "type": "string", "maxLength": 6, "default": "" },
				{ "key": "bind", "type": "key", "default": "F10" } ] } ] })json");
		s.LoadAll(sd, root / "u8-values");

		// Per-setting maxLength: 3 CJK chars = 9 bytes, cap 8 cuts inside the 3rd.
		const std::string cjk = "\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97";
		CHECK(cjk.size() == 9);
		CHECK(s.SetValueWithResult("t.utf8", "name", nlohmann::json(cjk)).ok);
		if (const auto* stored = s.GetValue("t.utf8", "name")) {
			CHECK(StrictDumpOk(*stored));
			CHECK(stored->get<std::string>().size() == 6);  // backed off onto the boundary
			CHECK(stored->get<std::string>() == cjk.substr(0, 6));
		} else {
			CHECK(false);
		}

		// 4-byte sequence (emoji): cap 6 lands inside the 2nd of two.
		const std::string emoji = "\xF0\x9F\x9A\x80\xF0\x9F\x9A\x80";  // two U+1F680
		CHECK(s.SetValueWithResult("t.utf8", "emoji", nlohmann::json(emoji)).ok);
		if (const auto* stored = s.GetValue("t.utf8", "emoji")) {
			CHECK(StrictDumpOk(*stored));
			CHECK(stored->get<std::string>().size() == 4);
		} else {
			CHECK(false);
		}

		std::string wide;
		for (int i = 0; i < 86; ++i) wide += "\xE4\xB8\xAD";
		CHECK(wide.size() == 258);
		CHECK(s.SetValueWithResult("t.utf8", "long", nlohmann::json(wide)).ok);
		if (const auto* stored = s.GetValue("t.utf8", "long")) {
			CHECK(StrictDumpOk(*stored));
			CHECK(stored->get<std::string>().size() == 255);
		} else {
			CHECK(false);
		}

		// The 16-byte key-name cap takes the same treatment.
		std::string longBind;
		for (int i = 0; i < 8; ++i) longBind += "\xE4\xB8\xAD";  // 24 bytes
		CHECK(s.SetValueWithResult("t.utf8", "bind", nlohmann::json(longBind)).ok);
		if (const auto* stored = s.GetValue("t.utf8", "bind")) {
			CHECK(StrictDumpOk(*stored));
			CHECK(stored->get<std::string>().size() == 15);
		} else {
			CHECK(false);
		}

		// The aggregate projections the bridge and the persist path serialize.
		CHECK(StrictDumpOk(s.DataView()));
		CHECK(!s.DataJson().empty());
	}

	{
		const auto bootRoot = root / "developer-mode-bootstrap";
		const auto bootSchemas = bootRoot / "settings";
		const auto bootValues = bootRoot / "values";
		WriteFile(bootSchemas / "osfui.json", R"json({
			"id": "osfui",
			"groups": [{ "settings": [
				{ "key": "developerMode", "type": "bool", "default": false, "requires": "restart" }
			] }]
		})json");

		SettingsStore missing;
		missing.LoadAll(bootSchemas, bootValues);
		CHECK(missing.GetValue("osfui", "developerMode") &&
			*missing.GetValue("osfui", "developerMode") == false);

		WriteFile(bootValues / "osfui.json", R"json({ "developerMode": true })json");
		SettingsStore enabled;
		enabled.LoadAll(bootSchemas, bootValues);
		CHECK(enabled.GetValue("osfui", "developerMode") &&
			*enabled.GetValue("osfui", "developerMode") == true);

		WriteFile(bootValues / "osfui.json", R"json({ "developerMode": "yes" })json");
		SettingsStore malformed;
		malformed.LoadAll(bootSchemas, bootValues);
		CHECK(malformed.GetValue("osfui", "developerMode") &&
			*malformed.GetValue("osfui", "developerMode") == false);
	}

	// ---------------------------------------------------------------------------
	std::fprintf(stderr, "%d/%d checks passed\n", g_checks - g_failures, g_checks);
	fs::remove_all(root);
	return g_failures;
}
