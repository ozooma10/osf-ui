// Host-side integration tests for the settings web-push path (mcm-design.md
// §8.5): the REAL SettingsModule + MessageBridge + SettingsStore driven
// through actual `ui.command` envelopes, with a capturing SendFn standing in
// for the renderer. Covers subscribe-on-read (`settings.get`), the
// `settings.changed` push, and the `settings.data` re-broadcast on registry
// shape changes. Assert-style; process exit code is the failure count.

#include "runtime/MessageBridge.h"
#include "runtime/SettingsModule.h"

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

	struct Sent
	{
		std::string    view;
		std::string    type;
		nlohmann::json payload;
		std::string    requestId;  // top-level echo (protocol 1.0); "" = none
	};

	std::vector<Sent> g_sent;

	// Count/fetch captured native->web messages of one type for one view.
	std::vector<Sent> SentTo(std::string_view a_view, std::string_view a_type)
	{
		std::vector<Sent> out;
		for (const auto& s : g_sent) {
			if (s.view == a_view && s.type == a_type) {
				out.push_back(s);
			}
		}
		return out;
	}

	// Drive the bridge exactly like the renderer does: a raw ui.command
	// envelope from a source view. a_requestId non-empty = a correlated
	// request (protocol 1.0).
	void Command(OSFUI::MessageBridge& a_bridge, std::string_view a_view, nlohmann::json a_payload,
		std::string_view a_requestId = {})
	{
		nlohmann::json envelope = { { "type", "ui.command" }, { "payload", std::move(a_payload) } };
		if (!a_requestId.empty()) {
			envelope["requestId"] = a_requestId;
		}
		a_bridge.HandleWebMessage(a_view, envelope.dump());
	}
}

// core/Log.h declarations (real impl pulls game deps — stub).
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
	using namespace OSFUI;
	namespace fs = std::filesystem;

	const auto root = fs::temp_directory_path() / "osfui-settings-module-tests";
	fs::remove_all(root);
	const auto schemaDir = root / "settings";
	const auto valuesDir = root / "values";

	WriteFile(schemaDir / "t.alpha.json", R"json({
		"id": "t.alpha", "title": "Alpha Mod",
		"groups": [ { "label": "General", "settings": [
			{ "key": "enabled", "type": "bool",  "default": true },
			{ "key": "scale",   "type": "float", "default": 1.0, "min": 0.5, "max": 2.0 }
		] } ] })json");

	std::vector<std::string> coreHeard;  // subscriber #0, the runtime reaction
	SettingsModule module(schemaDir, valuesDir,
		[&](std::string_view a_mod, std::string_view a_key, const nlohmann::json&) {
			coreHeard.push_back(std::format("{}.{}", a_mod, a_key));
		});

	// OnStart replay with NO bridge: core listener fires, web push no-ops.
	module.OnStart();
	CHECK(coreHeard.size() == 2);
	CHECK(g_sent.empty());

	MessageBridge bridge([](std::string_view a_view, std::string_view a_json) {
		auto msg = nlohmann::json::parse(a_json, nullptr, false);
		g_sent.push_back({ std::string(a_view), msg.value("type", ""), msg.value("payload", nlohmann::json()),
			msg.value("requestId", "") });
	});
	module.RegisterCommands(bridge);

	// --- subscribe-on-read -----------------------------------------------------
	Command(bridge, "settingsview", { { "command", "settings.get" } });
	Command(bridge, "hudview", { { "command", "settings.get" } });
	CHECK(SentTo("settingsview", "settings.data").size() == 1);
	CHECK(SentTo("hudview", "settings.data").size() == 1);

	// --- settings.set: ack to caller, settings.changed to ALL subscribers ------
	g_sent.clear();
	Command(bridge, "settingsview", { { "command", "settings.set" }, { "mod", "t.alpha" }, { "key", "scale" }, { "value", 1.5 } });
	{
		const auto acks = SentTo("settingsview", "settings.ack");
		CHECK(acks.size() == 1 && acks[0].payload["ok"] == true);
		CHECK(SentTo("hudview", "settings.ack").empty());  // ack is caller-only

		const auto toSettings = SentTo("settingsview", "settings.changed");
		const auto toHud = SentTo("hudview", "settings.changed");
		CHECK(toSettings.size() == 1);
		CHECK(toHud.size() == 1);
		CHECK(toHud[0].payload["mod"] == "t.alpha" && toHud[0].payload["key"] == "scale" && toHud[0].payload["value"] == 1.5);

		// Write-behind: the commit pushed settings.changed immediately, but the
		// disk write (and its settings.persisted confirmation) waits for the flush.
		CHECK(SentTo("settingsview", "settings.persisted").empty());
	}

	// --- write-behind flush lands: settings.persisted to ALL subscribers ---------
	g_sent.clear();
	module.Store().FlushPersistence();  // the set above left alpha dirty
	for (const auto* view : { "settingsview", "hudview" }) {
		const auto persisted = SentTo(view, "settings.persisted");
		CHECK(persisted.size() == 1 && persisted[0].payload["mod"] == "t.alpha");
	}
	g_sent.clear();
	module.Store().FlushPersistence();  // nothing dirty — no push
	CHECK(g_sent.empty());

	// --- rejected set: ack ok:false, NO settings.changed ------------------------
	g_sent.clear();
	Command(bridge, "settingsview", { { "command", "settings.set" }, { "mod", "t.alpha" }, { "key", "scale" }, { "value", "huge" } });
	{
		const auto acks = SentTo("settingsview", "settings.ack");
		CHECK(acks.size() == 1 && acks[0].payload["ok"] == false);
		CHECK(SentTo("hudview", "settings.changed").empty());
	}

	// --- a non-subscriber can set (it never called settings.get) ----------------
	g_sent.clear();
	Command(bridge, "otherview", { { "command", "settings.set" }, { "mod", "t.alpha" }, { "key", "enabled" }, { "value", false } });
	CHECK(SentTo("otherview", "settings.ack").size() == 1);
	CHECK(SentTo("otherview", "settings.changed").empty());  // not subscribed
	CHECK(SentTo("hudview", "settings.changed").size() == 1);

	// --- settings.reset: ONE settings.data to every subscriber, no per-key spam --
	g_sent.clear();
	Command(bridge, "settingsview", { { "command", "settings.reset" }, { "mod", "t.alpha" }, { "key", "" } });
	CHECK(SentTo("settingsview", "settings.data").size() == 1);
	CHECK(SentTo("hudview", "settings.data").size() == 1);
	CHECK(SentTo("settingsview", "settings.changed").empty());  // superseded by the data re-send
	CHECK(SentTo("hudview", "settings.changed").empty());

	// A caller that never subscribed still gets the authoritative re-send.
	g_sent.clear();
	Command(bridge, "otherview", { { "command", "settings.reset" }, { "mod", "t.alpha" }, { "key", "" } });
	CHECK(SentTo("otherview", "settings.data").size() == 1);
	CHECK(SentTo("hudview", "settings.data").size() == 1);

	// --- runtime registration: replay + settings.data re-broadcast (§8.5) -------
	g_sent.clear();
	auto gamma = nlohmann::json::parse(R"json({
		"id": "t.gamma", "title": "Gamma (runtime)",
		"groups": [ { "label": "G", "settings": [
			{ "key": "level", "type": "int", "default": 1, "min": 0, "max": 10 }
		] } ] })json");
	CHECK(module.Store().RegisterSchema(gamma, SettingsStore::Source::kNative));
	{
		// Value replay reaches subscribers as settings.changed...
		const auto changed = SentTo("hudview", "settings.changed");
		CHECK(changed.size() == 1 && changed[0].payload["mod"] == "t.gamma");
		// ...and the shape change re-broadcasts the full registry to BOTH.
		for (const auto* view : { "settingsview", "hudview" }) {
			const auto data = SentTo(view, "settings.data");
			CHECK(data.size() == 1 && data[0].payload["mods"].size() == 2);
		}
		CHECK(SentTo("otherview", "settings.data").empty());  // never subscribed
	}

	// --- removal re-broadcasts too ----------------------------------------------
	g_sent.clear();
	CHECK(module.Store().RemoveMod("t.gamma"));
	{
		const auto data = SentTo("hudview", "settings.data");
		CHECK(data.size() == 1 && data[0].payload["mods"].size() == 1);
	}

	// --- PushHotkey: ui.hotkey to every subscriber (views filter on mod) ---------
	g_sent.clear();
	module.PushHotkey("t.alpha", "toggleHud");
	for (const auto* view : { "settingsview", "hudview" }) {
		const auto hotkeys = SentTo(view, "ui.hotkey");
		CHECK(hotkeys.size() == 1 && hotkeys[0].payload["mod"] == "t.alpha" && hotkeys[0].payload["key"] == "toggleHud");
	}
	CHECK(SentTo("otherview", "ui.hotkey").empty());  // never subscribed

	// --- OnViewDestroyed: a torn-down view stops receiving pushes -----------------
	g_sent.clear();
	module.OnViewDestroyed("hudview");
	CHECK(module.Store().Set("t.alpha", "scale", "0.75"));
	CHECK(SentTo("hudview", "settings.changed").empty());
	CHECK(SentTo("settingsview", "settings.changed").size() == 1);  // others unaffected

	// --- OnBridgeDown: pushes stop, nothing dangles -------------------------------
	g_sent.clear();
	module.OnBridgeDown();
	CHECK(module.Store().Set("t.alpha", "scale", "0.5"));  // direct native write (future ABI path)
	CHECK(g_sent.empty());

	// A fresh RegisterCommands starts a clean subscriber set (views reload and
	// re-subscribe; stale ids must not receive pushes).
	module.RegisterCommands(bridge);
	CHECK(module.Store().Set("t.alpha", "scale", "1.25"));
	CHECK(g_sent.empty());

	// --- schema hot-reload (mcm-design §12.1) --------------------------------------
	{
		Command(bridge, "settingsview", { { "command", "settings.get" } });  // re-subscribe

		// Baseline scan: seeds nothing new (the ctor snapshot already covers
		// alpha.json) and starts the 1 s cadence clock.
		module.PumpSchemaHotReload(10.0);

		// Edit alpha.json: retitle + add a setting. Bump mtime explicitly so
		// the test never depends on filesystem timestamp resolution.
		const auto alphaPath = schemaDir / "t.alpha.json";
		const auto oldTime = fs::last_write_time(alphaPath);
		WriteFile(alphaPath, R"json({
			"id": "t.alpha", "title": "Alpha Mod v2",
			"groups": [ { "label": "General", "settings": [
				{ "key": "enabled", "type": "bool",  "default": true },
				{ "key": "scale",   "type": "float", "default": 1.0, "min": 0.5, "max": 2.0 },
				{ "key": "fresh",   "type": "int",   "default": 7 }
			] } ] })json");
		fs::last_write_time(alphaPath, oldTime + std::chrono::seconds(2));

		// Within the cadence window: nothing happens yet.
		g_sent.clear();
		module.PumpSchemaHotReload(10.5);
		CHECK(g_sent.empty());

		// Past the window: reloaded — new schema pushed, values preserved.
		module.PumpSchemaHotReload(11.0);
		{
			const auto data = SentTo("settingsview", "settings.data");
			CHECK(data.size() == 1);
			const auto& mods = data[0].payload["mods"];
			CHECK(mods.size() == 1 && mods[0]["title"] == "Alpha Mod v2");
			CHECK(mods[0]["values"]["scale"] == 1.25);  // the 1.25 set above survived the reload
			CHECK(mods[0]["values"]["fresh"] == 7);     // the added setting is live at its default
		}

		// A NEW drop-in file registers on the next scan.
		WriteFile(schemaDir / "t.delta.json", R"json({
			"id": "t.delta", "groups": [ { "settings": [
				{ "key": "on", "type": "bool", "default": false }
			] } ] })json");
		g_sent.clear();
		module.PumpSchemaHotReload(12.0);
		{
			const auto data = SentTo("settingsview", "settings.data");
			CHECK(!data.empty() && data.back().payload["mods"].size() == 2);
		}

		// Deleting the file drops the mod (drop-ins only) and re-broadcasts.
		fs::remove(schemaDir / "t.delta.json");
		g_sent.clear();
		module.PumpSchemaHotReload(13.0);
		{
			const auto data = SentTo("settingsview", "settings.data");
			CHECK(data.size() == 1 && data[0].payload["mods"].size() == 1);
		}

		// A runtime (native) registration outranks the file both ways: a
		// same-id file appearing neither replaces it nor, when deleted again,
		// removes it.
		CHECK(module.Store().RegisterSchema(nlohmann::json::parse(R"json({
			"id": "t.epsilon", "title": "Native Epsilon",
			"groups": [ { "settings": [ { "key": "n", "type": "int", "default": 1 } ] } ] })json"),
			SettingsStore::Source::kNative));
		WriteFile(schemaDir / "t.epsilon.json", R"json({
			"id": "t.epsilon", "title": "File Impostor",
			"groups": [ { "settings": [ { "key": "n", "type": "int", "default": 99 } ] } ] })json");
		module.PumpSchemaHotReload(14.0);
		{
			const auto* n = module.Store().GetValue("t.epsilon", "n");
			CHECK(n && *n == 1);  // native schema untouched by the file (default stays 1, not 99)
		}
		fs::remove(schemaDir / "t.epsilon.json");
		module.PumpSchemaHotReload(15.0);
		CHECK(module.Store().GetValue("t.epsilon", "n") != nullptr);  // file deletion can't remove a native mod
	}

	// --- items 5 + 11 (protocol 1.0): ack shape, requestId echo, reset failure ---
	{
		// settings.set ack carries the authoritative post-clamp value...
		g_sent.clear();
		Command(bridge, "settingsview", { { "command", "settings.set" }, { "mod", "t.alpha" }, { "key", "scale" }, { "value", 1.75 } }, "q1");
		{
			const auto acks = SentTo("settingsview", "settings.ack");
			CHECK(acks.size() == 1 && acks[0].payload["ok"] == true && acks[0].payload["value"] == 1.75);
			CHECK(acks.size() == 1 && acks[0].requestId == "q1");  // top-level echo
			CHECK(acks.size() == 1 && !acks[0].payload.contains("code"));
		}
		// ...including a CLAMPED commit (ok:true, the stored value, no code).
		g_sent.clear();
		Command(bridge, "settingsview", { { "command", "settings.set" }, { "mod", "t.alpha" }, { "key", "scale" }, { "value", 99.0 } });
		{
			const auto acks = SentTo("settingsview", "settings.ack");
			CHECK(acks.size() == 1 && acks[0].payload["ok"] == true && acks[0].payload["value"] == 2.0);
			CHECK(acks.size() == 1 && acks[0].requestId.empty());  // fire-and-forget: no echo
		}
		// Failures carry the machine code.
		g_sent.clear();
		Command(bridge, "settingsview", { { "command", "settings.set" }, { "mod", "t.alpha" }, { "key", "scale" }, { "value", "huge" } });
		CHECK(SentTo("settingsview", "settings.ack").size() == 1 &&
		      SentTo("settingsview", "settings.ack")[0].payload["code"] == "invalid-value");
		g_sent.clear();
		Command(bridge, "settingsview", { { "command", "settings.set" }, { "mod", "t.alpha" }, { "key", "nope" }, { "value", 1 } });
		CHECK(SentTo("settingsview", "settings.ack").size() == 1 &&
		      SentTo("settingsview", "settings.ack")[0].payload["code"] == "unknown-setting");
		g_sent.clear();
		Command(bridge, "settingsview", { { "command", "settings.set" }, { "mod", "t.alpha" }, { "key", "scale" } });  // no value field
		CHECK(SentTo("settingsview", "settings.ack").size() == 1 &&
		      SentTo("settingsview", "settings.ack")[0].payload["code"] == "invalid-value");

		// settings.reset with a requestId: the settings.data REPLY echoes it
		// (that is what resolves osfui.request("settings.reset")).
		g_sent.clear();
		Command(bridge, "settingsview", { { "command", "settings.reset" }, { "mod", "t.alpha" }, { "key", "" } }, "q2");
		{
			const auto data = SentTo("settingsview", "settings.data");
			CHECK(data.size() == 1 && data[0].requestId == "q2");
		}
		// A failed reset is no longer silent for a correlated caller...
		g_sent.clear();
		Command(bridge, "settingsview", { { "command", "settings.reset" }, { "mod", "nope.mod" }, { "key", "" } }, "q3");
		{
			const auto results = SentTo("settingsview", "ui.result");
			CHECK(results.size() == 1 && results[0].payload["ok"] == false &&
			      results[0].payload["code"] == "unknown-setting" && results[0].requestId == "q3");
			CHECK(SentTo("settingsview", "settings.data").empty());
		}
		// ...and stays silent fire-and-forget.
		g_sent.clear();
		Command(bridge, "settingsview", { { "command", "settings.reset" }, { "mod", "nope.mod" }, { "key", "" } });
		CHECK(g_sent.empty());
	}

	// --- item 11: key-typed settings.changed carries recomputed conflicts --------
	{
		module.Store().SetKeyNameResolver([](std::string_view a_name) -> std::uint32_t {
			if (a_name == "F6") return 0x75;
			if (a_name == "F7") return 0x76;
			return 0;
		});
		CHECK(module.Store().RegisterSchema(nlohmann::json::parse(R"json({
			"id": "t.keys", "title": "Keys",
			"groups": [ { "settings": [
				{ "key": "one", "type": "key", "default": "F6" },
				{ "key": "two", "type": "key", "default": "F7" }
			] } ] })json"),
			SettingsStore::Source::kNative));

		// Rebind INTO a collision: the push names the partner.
		g_sent.clear();
		Command(bridge, "settingsview", { { "command", "settings.set" }, { "mod", "t.keys" }, { "key", "one" }, { "value", "F7" } });
		{
			const auto changed = SentTo("settingsview", "settings.changed");
			CHECK(changed.size() == 1 && changed[0].payload.contains("conflicts"));
			CHECK(changed.size() == 1 && changed[0].payload["conflicts"].size() == 1 &&
			      changed[0].payload["conflicts"][0]["mod"] == "t.keys" &&
			      changed[0].payload["conflicts"][0]["key"] == "two");
		}
		// Rebind OUT again: conflicts present but EMPTY (the badge-clearing signal).
		g_sent.clear();
		Command(bridge, "settingsview", { { "command", "settings.set" }, { "mod", "t.keys" }, { "key", "one" }, { "value", "F6" } });
		{
			const auto changed = SentTo("settingsview", "settings.changed");
			CHECK(changed.size() == 1 && changed[0].payload.contains("conflicts"));
			CHECK(changed.size() == 1 && changed[0].payload["conflicts"].empty());
		}
		// Non-key settings never carry the field.
		g_sent.clear();
		Command(bridge, "settingsview", { { "command", "settings.set" }, { "mod", "t.alpha" }, { "key", "enabled" }, { "value", true } });
		{
			const auto changed = SentTo("settingsview", "settings.changed");
			CHECK(changed.size() == 1 && !changed[0].payload.contains("conflicts"));
		}
	}

	// Mirror the runtime's teardown order: the bridge (declared after `module`)
	// destructs FIRST, and ~SettingsStore's final flush fires the persist
	// listeners — without this, dirty mods left by the sections above would
	// push into a dangling bridge pointer.
	module.OnBridgeDown();

	// ---------------------------------------------------------------------------
	std::fprintf(stderr, "%d/%d checks passed\n", g_checks - g_failures, g_checks);
	fs::remove_all(root);
	return g_failures;
}
