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
	WriteFile(schemaDir / "t.alpha.json", R"json({
		"id": "t.alpha", "title": "Alpha Mod",
		"groups": [ { "label": "Keys", "settings": [
			{ "key": "toggleHud",  "type": "key",  "default": "F6" },
			{ "key": "screenshot", "type": "key",  "default": "F7" },
			{ "key": "enabled",    "type": "bool", "default": true }
		] } ] })json");
	WriteFile(schemaDir / "t.beta.json", R"json({
		"id": "t.beta", "title": "Beta Mod",
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
		CHECK(std::find(fired.begin(), fired.end(), "t.alpha.toggleHud") != fired.end());
		CHECK(std::find(fired.begin(), fired.end(), "t.beta.openMenu") != fired.end());

		CHECK(DrainAll(svc).empty());  // drained once

		svc.OnKeyDown(vkF8);  // nothing bound
		CHECK(DrainAll(svc).empty());

		svc.OnKeyDown(vkF7);
		const auto f7 = DrainAll(svc);
		CHECK(f7.size() == 1 && f7[0] == "t.alpha.screenshot");
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
		CHECK(fired[0] == "t.alpha.screenshot");
	}

	// --- registry rebuild on rebind (store.Set through the change listener) ------
	{
		CHECK(store.Set("t.alpha", "toggleHud", "\"F8\""));

		svc.OnKeyDown(vkF6);  // only beta remains on F6
		auto fired = DrainAll(svc);
		CHECK(fired.size() == 1 && fired[0] == "t.beta.openMenu");

		svc.OnKeyDown(vkF8);  // alpha moved here
		fired = DrainAll(svc);
		CHECK(fired.size() == 1 && fired[0] == "t.alpha.toggleHud");
	}

	// --- an unresolvable stored name simply doesn't bind --------------------------
	{
		CHECK(store.Set("t.alpha", "toggleHud", "\"NotAKey\""));  // valid string, no VK
		svc.OnKeyDown(vkF8);
		CHECK(DrainAll(svc).empty());
		CHECK(store.Set("t.alpha", "toggleHud", "\"F8\""));  // restore
	}

	// --- a non-key commit does not disturb the registry ---------------------------
	{
		CHECK(store.Set("t.alpha", "enabled", "false"));
		svc.OnKeyDown(vkF8);
		CHECK(DrainAll(svc).size() == 1);
	}

	// --- registry shape changes: late registration binds, removal unbinds --------
	{
		CHECK(store.RegisterSchema(nlohmann::json::parse(R"json({
			"id": "t.gamma", "title": "Gamma",
			"groups": [ { "settings": [
				{ "key": "quickSlot", "type": "key", "default": "F6" }
			] } ] })json"),
			SettingsStore::Source::kNative));

		svc.OnKeyDown(vkF6);
		auto fired = DrainAll(svc);
		CHECK(fired.size() == 2);  // beta + gamma

		CHECK(store.RemoveMod("t.gamma"));
		CHECK(store.RemoveMod("t.beta"));
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
		WriteFile(schemaDir2 / "t.delta.json", R"json({
			"id": "t.delta", "title": "Delta Mod",
			"groups": [ { "settings": [
				{ "key": "boundA", "type": "key", "default": "F6" },
				{ "key": "boundB", "type": "key", "default": "F6" },
				{ "key": "unique", "type": "key", "default": "F9" },
				{ "key": "grave",  "type": "key", "default": "Tilde" }
			] } ] })json");
		WriteFile(schemaDir2 / "t.epsilon.json", R"json({
			"id": "t.epsilon", "title": "Epsilon Mod",
			"groups": [ { "settings": [
				{ "key": "console", "type": "key", "default": "Grave" }
			] } ] })json");

		SettingsStore s2;
		s2.SetKeyNameResolver(ResolveKeyName);
		s2.LoadAll(schemaDir2, root2 / "values");

		auto data = s2.Data();
		// Same-mod duplicate: both sides badge each other.
		CHECK(ConflictsOf(FindEmittedSetting(data, "t.delta", "boundA")) == std::vector<std::string>{ "t.delta.boundB" });
		CHECK(ConflictsOf(FindEmittedSetting(data, "t.delta", "boundB")) == std::vector<std::string>{ "t.delta.boundA" });
		// Cross-mod alias collision (Tilde vs Grave = one VK), with titles.
		CHECK(ConflictsOf(FindEmittedSetting(data, "t.delta", "grave")) == std::vector<std::string>{ "t.epsilon.console" });
		CHECK(ConflictsOf(FindEmittedSetting(data, "t.epsilon", "console")) == std::vector<std::string>{ "t.delta.grave" });
		{
			const auto* setting = FindEmittedSetting(data, "t.epsilon", "console");
			CHECK(setting && setting->at("conflicts")[0].value("title", "") == "Delta Mod");
		}
		// Unique binding: no conflicts field at all.
		CHECK(!FindEmittedSetting(data, "t.delta", "unique")->contains("conflicts"));

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
		CHECK(names(s2.ConflictsFor(vkF6, "t.delta", "unique")) == (std::vector<std::string>{ "t.delta.boundA", "t.delta.boundB" }));
		// Re-capturing F6 for a setting already on F6: self is excluded even
		// though its stored value still resolves to the same VK.
		CHECK(names(s2.ConflictsFor(vkF6, "t.delta", "boundA")) == std::vector<std::string>{ "t.delta.boundB" });
		// Alias-aware (Tilde vs Grave = one VK), and titles come through.
		{
			const auto conflicts = s2.ConflictsFor(ResolveKeyName("Tilde"), "t.delta", "grave");
			CHECK(names(conflicts) == std::vector<std::string>{ "t.epsilon.console" });
			CHECK(!conflicts.empty() && conflicts[0].value("title", "") == "Epsilon Mod");
		}
		// A key nobody holds, and a key held only by self: empty either way.
		CHECK(s2.ConflictsFor(ResolveKeyName("F12"), "t.delta", "unique").empty());
		CHECK(s2.ConflictsFor(ResolveKeyName("F9"), "t.delta", "unique").empty());
		// vk 0 (unresolvable capture) never conflicts.
		CHECK(s2.ConflictsFor(0, "t.delta", "unique").empty());

		// A rebind away clears both sides on the next Data() — i.e. the
		// annotation lives on the emitted COPY; the stored schema is never
		// mutated.
		CHECK(s2.Set("t.delta", "boundB", "\"F10\""));
		data = s2.Data();
		CHECK(!FindEmittedSetting(data, "t.delta", "boundA")->contains("conflicts"));
		CHECK(!FindEmittedSetting(data, "t.delta", "boundB")->contains("conflicts"));

		// With the remaining collision (grave/console) also rebound away, no
		// conflict data survives anywhere in the document.
		CHECK(s2.Set("t.delta", "grave", "\"F11\""));
		CHECK(s2.DataJson().find("\"conflicts\"") == std::string::npos);

		// Without a resolver, Data() emits no conflict data and ConflictsFor
		// finds none (host defensive default; the composition root always
		// wires one).
		SettingsStore s3;
		s3.LoadAll(schemaDir2, root2 / "values3");
		CHECK(s3.DataJson().find("\"conflicts\"") == std::string::npos);
		CHECK(s3.ConflictsFor(vkF6, "t.delta", "unique").empty());
	}

	// --- vanilla hotkeys (§9 v1): "@game" pseudo-entries in the grouping ----------
	{
		const auto root3 = root / "vanilla";
		WriteFile(root3 / "settings" / "t.eta.json", R"json({
			"id": "t.eta", "title": "Eta Mod",
			"groups": [ { "settings": [
				{ "key": "globalSpace", "type": "key", "default": "Space" }
			] } ] })json");
		WriteFile(root3 / "settings" / "t.zeta.json", R"json({
			"id": "t.zeta", "title": "Zeta Mod",
			"inputContexts": [
				{ "id": "scene", "label": "During scenes", "blocksGameplay": true },
				{ "id": "scene", "label": "Ignored duplicate", "blocksGameplay": false },
				{ "id": "gameplay", "blocksGameplay": true },
				{ "id": "bad id", "blocksGameplay": true }
			],
			"groups": [ { "settings": [
				{ "key": "save",    "type": "key", "default": "F5" },
				{ "key": "other",   "type": "key", "default": "F7" },
				{ "key": "scene",   "type": "key", "default": "Space", "inputContext": "scene" },
				{ "key": "unknown", "type": "key", "default": "F9", "inputContext": "missing" },
				{ "key": "invalid", "type": "key", "default": "Grave", "inputContext": "bad id" }
			] } ] })json");

		SettingsStore s5;
		HotkeyService svc5;
		s5.SetKeyNameResolver(ResolveKeyName);
		s5.LoadAll(root3 / "settings", root3 / "values");
		s5.SetVanillaKeys({
			{ "QuickSave", "Starfield (Quicksave)", ResolveKeyName("F5"), "F5" },
			{ "QuickLoad", "Starfield (Quickload)", ResolveKeyName("F9"), "F9" },
			{ "Console", "Starfield (Console)", ResolveKeyName("Grave"), "Grave" },
			{ "Jump", "Starfield (Jump)", ResolveKeyName("Space"), "Space" },
		});

		const auto data = s5.Data();
		CHECK(data.contains("vanillaKeys") && data["vanillaKeys"].size() == 4);
		CHECK(data["vanillaKeys"][0] == (nlohmann::json{
			{ "event", "QuickSave" }, { "title", "Starfield (Quicksave)" }, { "name", "F5" } }));

		// Ordinary and fallback gameplay contexts still warn against @game.
		CHECK(ConflictsOf(FindEmittedSetting(data, "t.zeta", "save")) ==
			std::vector<std::string>{ "@game.QuickSave" });
		CHECK(ConflictsOf(FindEmittedSetting(data, "t.zeta", "unknown")) ==
			std::vector<std::string>{ "@game.QuickLoad" });
		CHECK(ConflictsOf(FindEmittedSetting(data, "t.zeta", "invalid")) ==
			std::vector<std::string>{ "@game.Console" });
		CHECK(!FindEmittedSetting(data, "t.zeta", "other")->contains("conflicts"));

		// The first valid duplicate context wins: scene omits @game.Jump but
		// still reports the other mod on Space.
		CHECK(ConflictsOf(FindEmittedSetting(data, "t.zeta", "scene")) ==
			std::vector<std::string>{ "t.eta.globalSpace" });
		CHECK(ConflictsOf(FindEmittedSetting(data, "t.eta", "globalSpace")) ==
			(std::vector<std::string>{ "@game.Jump", "t.zeta.scene" }));

		// Capture-time filtering uses the target setting's authored context.
		const nlohmann::json sceneCapture{
			{ "conflicts", s5.ConflictsFor(ResolveKeyName("Space"), "t.zeta", "scene") } };
		CHECK(ConflictsOf(&sceneCapture) == std::vector<std::string>{ "t.eta.globalSpace" });
		const nlohmann::json fallbackCapture{
			{ "conflicts", s5.ConflictsFor(ResolveKeyName("F9"), "t.zeta", "unknown") } };
		CHECK(ConflictsOf(&fallbackCapture) == std::vector<std::string>{ "@game.QuickLoad" });

		// Metadata does not change dispatch: both mod bindings still fan out.
		svc5.Rebuild(s5);
		svc5.OnKeyDown(ResolveKeyName("Space"));
		const auto fired = DrainAll(svc5);
		CHECK(fired.size() == 2);
		CHECK(std::find(fired.begin(), fired.end(), "t.eta.globalSpace") != fired.end());
		CHECK(std::find(fired.begin(), fired.end(), "t.zeta.scene") != fired.end());

		// A malformed top-level context table also falls back to gameplay.
		const auto root4 = root / "bad-context-table";
		WriteFile(root4 / "settings" / "t.theta.json", R"json({
			"id": "t.theta", "inputContexts": { "id": "scene", "blocksGameplay": true },
			"groups": [ { "settings": [
				{ "key": "scene", "type": "key", "default": "Space", "inputContext": "scene" }
			] } ] })json");
		SettingsStore s6;
		s6.SetKeyNameResolver(ResolveKeyName);
		s6.LoadAll(root4 / "settings", root4 / "values");
		s6.SetVanillaKeys({ { "Jump", "Starfield (Jump)", ResolveKeyName("Space"), "Space" } });
		CHECK(ConflictsOf(FindEmittedSetting(s6.Data(), "t.theta", "scene")) ==
			std::vector<std::string>{ "@game.Jump" });
	}

	fs::remove_all(root);
	std::fprintf(stderr, "hotkey_service_tests: %d checks, %d failure(s)\n", g_checks, g_failures);
	return g_failures;
}
