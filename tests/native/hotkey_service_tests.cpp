// Host-side tests for the HotkeyService core (docs/mcm-design.md §9): the
// REAL src/runtime/HotkeyService.cpp + SettingsStore + input's ResolveKeyName,
// wired exactly like Runtime::BuildModules — registry rebuild on rebind and
// registry shape change, suppression while the overlay captures / a rebind is
// armed, duplicate-binding fan-out, and the informational conflict data
// embedded in SettingsStore::Data(). Assert-style; process exit code is the
// failure count.

#include "runtime/HotkeyService.h"
#include "runtime/SettingsStore.h"

#include "core/Log.h"

namespace
{
	int g_failures = 0;
	int g_checks = 0;

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

	// Drain into "mod.key" strings, FIFO.
	std::vector<std::string> DrainAll(OSFUI::HotkeyService& a_svc)
	{
		std::vector<std::string> fired;
		a_svc.Drain([&](const std::string& a_mod, const std::string& a_key) {
			fired.push_back(std::format("{}.{}", a_mod, a_key));
		});
		return fired;
	}

	// The emitted schema object for one setting of one mod in a Data()
	// document, or nullptr.
	const nlohmann::json* FindEmittedSetting(const nlohmann::json& a_data, std::string_view a_mod, std::string_view a_key)
	{
		// Iterate by reference into a_data (json::value() would return
		// temporaries and the returned pointer must outlive this call).
		for (const auto& mod : a_data.at("mods")) {
			if (mod.value("id", "") != a_mod) {
				continue;
			}
			const auto& schema = mod.at("schema");
			const auto groups = schema.find("groups");
			if (groups == schema.end() || !groups->is_array()) {
				continue;
			}
			for (const auto& group : *groups) {
				const auto settings = group.find("settings");
				if (settings == group.end() || !settings->is_array()) {
					continue;
				}
				for (const auto& setting : *settings) {
					if (setting.is_object() && setting.value("key", "") == a_key) {
						return &setting;
					}
				}
			}
		}
		return nullptr;
	}

	// The setting's conflicts as "mod.key" strings (order-insensitive checks).
	std::vector<std::string> ConflictsOf(const nlohmann::json* a_setting)
	{
		std::vector<std::string> out;
		if (a_setting && a_setting->contains("conflicts")) {
			for (const auto& c : a_setting->at("conflicts")) {
				out.push_back(std::format("{}.{}", c.value("mod", ""), c.value("key", "")));
			}
		}
		std::sort(out.begin(), out.end());
		return out;
	}
}

// core/Log.h declarations (real impl pulls game deps — stub, as in the other
// suites).
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
	using OSFUI::HotkeyService;
	using OSFUI::ResolveKeyName;
	using OSFUI::SettingsStore;
	namespace fs = std::filesystem;

	const auto root = fs::temp_directory_path() / "osfui-hotkey-service-tests";
	fs::remove_all(root);
	const auto schemaDir = root / "settings";
	const auto valuesDir = root / "values";

	// alpha.toggleHud and beta.openMenu share F6 by default (the informational
	// conflict case); alpha.screenshot is unique on F7.
	WriteFile(schemaDir / "alpha.json", R"json({
		"id": "alpha", "title": "Alpha Mod",
		"groups": [ { "label": "Keys", "settings": [
			{ "key": "toggleHud",  "type": "key",  "default": "F6" },
			{ "key": "screenshot", "type": "key",  "default": "F7" },
			{ "key": "enabled",    "type": "bool", "default": true }
		] } ] })json");
	WriteFile(schemaDir / "beta.json", R"json({
		"id": "beta", "title": "Beta Mod",
		"groups": [ { "label": "Keys", "settings": [
			{ "key": "openMenu", "type": "key", "default": "F6" }
		] } ] })json");

	SettingsStore store;
	HotkeyService svc;
	bool suppressed = false;
	svc.SetSuppression([&] { return suppressed; });

	// Exactly the Runtime::BuildModules wiring: rebuild on any key-typed
	// commit and on registry shape change; conflicts share ResolveKeyName.
	store.SetKeyNameResolver(ResolveKeyName);
	store.AddChangeListener([&](std::string_view a_mod, std::string_view a_key, const nlohmann::json&) {
		if (store.GetSettingType(a_mod, a_key) == "key") {
			svc.Rebuild(store);
		}
	});
	store.AddRegistryListener([&] { svc.Rebuild(store); });

	store.LoadAll(schemaDir, valuesDir);
	svc.Rebuild(store);  // composition-time build (LoadAll defers notifications)

	const auto vkF6 = ResolveKeyName("F6");
	const auto vkF7 = ResolveKeyName("F7");
	const auto vkF8 = ResolveKeyName("F8");

	// --- dispatch: duplicate bindings fan out; unbound keys are silent -----------
	{
		svc.OnKeyDown(vkF6);
		const auto fired = DrainAll(svc);
		CHECK(fired.size() == 2);
		CHECK(std::find(fired.begin(), fired.end(), "alpha.toggleHud") != fired.end());
		CHECK(std::find(fired.begin(), fired.end(), "beta.openMenu") != fired.end());

		CHECK(DrainAll(svc).empty());  // drained once

		svc.OnKeyDown(vkF8);  // nothing bound
		CHECK(DrainAll(svc).empty());

		svc.OnKeyDown(vkF7);
		const auto f7 = DrainAll(svc);
		CHECK(f7.size() == 1 && f7[0] == "alpha.screenshot");
	}

	// --- suppression: captured/armed presses never fire --------------------------
	{
		suppressed = true;
		svc.OnKeyDown(vkF6);
		CHECK(DrainAll(svc).empty());

		suppressed = false;
		svc.OnKeyDown(vkF6);
		CHECK(DrainAll(svc).size() == 2);
	}

	// --- FIFO across presses ------------------------------------------------------
	{
		svc.OnKeyDown(vkF7);
		svc.OnKeyDown(vkF6);
		const auto fired = DrainAll(svc);
		CHECK(fired.size() == 3);
		CHECK(fired[0] == "alpha.screenshot");
	}

	// --- registry rebuild on rebind (store.Set through the change listener) ------
	{
		CHECK(store.Set("alpha", "toggleHud", "\"F8\""));

		svc.OnKeyDown(vkF6);  // only beta remains on F6
		auto fired = DrainAll(svc);
		CHECK(fired.size() == 1 && fired[0] == "beta.openMenu");

		svc.OnKeyDown(vkF8);  // alpha moved here
		fired = DrainAll(svc);
		CHECK(fired.size() == 1 && fired[0] == "alpha.toggleHud");
	}

	// --- an unresolvable stored name simply doesn't bind --------------------------
	{
		CHECK(store.Set("alpha", "toggleHud", "\"NotAKey\""));  // valid string, no VK
		svc.OnKeyDown(vkF8);
		CHECK(DrainAll(svc).empty());
		CHECK(store.Set("alpha", "toggleHud", "\"F8\""));  // restore
	}

	// --- a non-key commit does not disturb the registry ---------------------------
	{
		CHECK(store.Set("alpha", "enabled", "false"));
		svc.OnKeyDown(vkF8);
		CHECK(DrainAll(svc).size() == 1);
	}

	// --- registry shape changes: late registration binds, removal unbinds --------
	{
		CHECK(store.RegisterSchema(nlohmann::json::parse(R"json({
			"id": "gamma", "title": "Gamma",
			"groups": [ { "settings": [
				{ "key": "quickSlot", "type": "key", "default": "F6" }
			] } ] })json"),
			SettingsStore::Source::kNative));

		svc.OnKeyDown(vkF6);
		auto fired = DrainAll(svc);
		CHECK(fired.size() == 2);  // beta + gamma

		CHECK(store.RemoveMod("gamma"));
		CHECK(store.RemoveMod("beta"));
		svc.OnKeyDown(vkF6);
		CHECK(DrainAll(svc).empty());
	}

	// --- conflict data in Data(): informational, both sides, alias-aware ---------
	{
		// Fresh store so this section owns its bindings. delta.grave uses the
		// "Tilde" alias of epsilon.console's "Grave" — same VK, so the
		// grouping must resolve names, not compare strings.
		const auto root2 = root / "conflicts";
		const auto schemaDir2 = root2 / "settings";
		WriteFile(schemaDir2 / "delta.json", R"json({
			"id": "delta", "title": "Delta Mod",
			"groups": [ { "settings": [
				{ "key": "boundA", "type": "key", "default": "F6" },
				{ "key": "boundB", "type": "key", "default": "F6" },
				{ "key": "unique", "type": "key", "default": "F9" },
				{ "key": "grave",  "type": "key", "default": "Tilde" }
			] } ] })json");
		WriteFile(schemaDir2 / "epsilon.json", R"json({
			"id": "epsilon", "title": "Epsilon Mod",
			"groups": [ { "settings": [
				{ "key": "console", "type": "key", "default": "Grave" }
			] } ] })json");

		SettingsStore s2;
		s2.SetKeyNameResolver(ResolveKeyName);
		s2.LoadAll(schemaDir2, root2 / "values");

		auto data = s2.Data();
		// Same-mod duplicate: both sides badge each other.
		CHECK(ConflictsOf(FindEmittedSetting(data, "delta", "boundA")) == std::vector<std::string>{ "delta.boundB" });
		CHECK(ConflictsOf(FindEmittedSetting(data, "delta", "boundB")) == std::vector<std::string>{ "delta.boundA" });
		// Cross-mod alias collision (Tilde vs Grave = one VK), with titles.
		CHECK(ConflictsOf(FindEmittedSetting(data, "delta", "grave")) == std::vector<std::string>{ "epsilon.console" });
		CHECK(ConflictsOf(FindEmittedSetting(data, "epsilon", "console")) == std::vector<std::string>{ "delta.grave" });
		{
			const auto* setting = FindEmittedSetting(data, "epsilon", "console");
			CHECK(setting && setting->at("conflicts")[0].value("title", "") == "Delta Mod");
		}
		// Unique binding: no conflicts field at all.
		CHECK(!FindEmittedSetting(data, "delta", "unique")->contains("conflicts"));

		// --- ConflictsFor(): the live-warn half (capture-time lookup) --------
		// Same store state; the runtime calls this with the just-captured VK
		// BEFORE the view commits, so the setting being rebound still holds
		// its OLD value — hence the explicit self-exclusion.
		const auto names = [](const nlohmann::json& a_conflicts) {
			std::vector<std::string> out;
			for (const auto& c : a_conflicts) {
				out.push_back(std::format("{}.{}", c.value("mod", ""), c.value("key", "")));
			}
			std::sort(out.begin(), out.end());
			return out;
		};
		// Rebinding delta.unique onto F6 would collide with both holders.
		CHECK(names(s2.ConflictsFor(vkF6, "delta", "unique")) == (std::vector<std::string>{ "delta.boundA", "delta.boundB" }));
		// Re-capturing F6 for a setting already on F6: self is excluded even
		// though its stored value still resolves to the same VK.
		CHECK(names(s2.ConflictsFor(vkF6, "delta", "boundA")) == std::vector<std::string>{ "delta.boundB" });
		// Alias-aware (Tilde vs Grave = one VK), and titles come through.
		{
			const auto conflicts = s2.ConflictsFor(ResolveKeyName("Tilde"), "delta", "grave");
			CHECK(names(conflicts) == std::vector<std::string>{ "epsilon.console" });
			CHECK(!conflicts.empty() && conflicts[0].value("title", "") == "Epsilon Mod");
		}
		// A key nobody holds, and a key held only by self: empty either way.
		CHECK(s2.ConflictsFor(ResolveKeyName("F12"), "delta", "unique").empty());
		CHECK(s2.ConflictsFor(ResolveKeyName("F9"), "delta", "unique").empty());
		// vk 0 (unresolvable capture) never conflicts.
		CHECK(s2.ConflictsFor(0, "delta", "unique").empty());

		// A rebind away clears both sides on the next Data() — i.e. the
		// annotation lives on the emitted COPY; the stored schema is never
		// mutated.
		CHECK(s2.Set("delta", "boundB", "\"F10\""));
		data = s2.Data();
		CHECK(!FindEmittedSetting(data, "delta", "boundA")->contains("conflicts"));
		CHECK(!FindEmittedSetting(data, "delta", "boundB")->contains("conflicts"));

		// With the remaining collision (grave/console) also rebound away, no
		// conflict data survives anywhere in the document.
		CHECK(s2.Set("delta", "grave", "\"F11\""));
		CHECK(s2.DataJson().find("\"conflicts\"") == std::string::npos);

		// Without a resolver, Data() emits no conflict data and ConflictsFor
		// finds none (host defensive default; the composition root always
		// wires one).
		SettingsStore s3;
		s3.LoadAll(schemaDir2, root2 / "values3");
		CHECK(s3.DataJson().find("\"conflicts\"") == std::string::npos);
		CHECK(s3.ConflictsFor(vkF6, "delta", "unique").empty());
	}

	// --- vanilla hotkeys (§9 v1): "@game" pseudo-entries in the grouping ----------
	{
		const auto root3 = root / "vanilla";
		WriteFile(root3 / "settings" / "zeta.json", R"json({
			"id": "zeta", "title": "Zeta Mod",
			"groups": [ { "settings": [
				{ "key": "save",  "type": "key", "default": "F5" },
				{ "key": "other", "type": "key", "default": "F7" }
			] } ] })json");

		SettingsStore s5;
		HotkeyService svc5;
		s5.SetKeyNameResolver(ResolveKeyName);
		s5.LoadAll(root3 / "settings", root3 / "values");
		s5.SetVanillaKeys({
			{ "QuickSave", "Starfield (Quicksave)", ResolveKeyName("F5") },
			{ "Console", "Starfield (Console)", ResolveKeyName("Grave") },
		});

		// Data(): the colliding setting badges against the game; the vanilla
		// entry is never a *self*, so nothing else in the document changes.
		const auto data = s5.Data();
		const auto* save = FindEmittedSetting(data, "zeta", "save");
		CHECK(save && save->contains("conflicts") && save->at("conflicts").size() == 1);
		CHECK(save && save->at("conflicts")[0].value("mod", "") == "@game");
		CHECK(save && save->at("conflicts")[0].value("key", "") == "QuickSave");
		CHECK(save && save->at("conflicts")[0].value("title", "") == "Starfield (Quicksave)");
		CHECK(!FindEmittedSetting(data, "zeta", "other")->contains("conflicts"));

		// Capture-time live-warn sees the game side too.
		const nlohmann::json wrapped{ { "conflicts", s5.ConflictsFor(ResolveKeyName("Grave"), "zeta", "other") } };
		CHECK(ConflictsOf(&wrapped) == std::vector<std::string>{ "@game.Console" });

		// The hotkey registry is untouched: vanilla keys are conflict data,
		// not dispatchable bindings.
		svc5.Rebuild(s5);
		svc5.OnKeyDown(ResolveKeyName("Grave"));  // vanilla-only key
		CHECK(DrainAll(svc5).empty());
		svc5.OnKeyDown(ResolveKeyName("F5"));     // shared key: only the MOD fires
		const auto fired = DrainAll(svc5);
		CHECK(fired.size() == 1 && fired[0] == "zeta.save");
	}

	fs::remove_all(root);
	std::fprintf(stderr, "hotkey_service_tests: %d checks, %d failure(s)\n", g_checks, g_failures);
	return g_failures;
}
