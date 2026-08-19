
#include "Bridge/MessageBridge.h"
#include "Settings/SettingsModule.h"

#include "Core/Log.h"
#include "check.h"

namespace
{

	void WriteFile(const std::filesystem::path& a_path, std::string_view a_text)
	{
		std::filesystem::create_directories(a_path.parent_path());
		std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
		out << a_text;
	}

	struct Sent
	{
		std::string    view;
		std::string    kind;     // ready | state | event | reply | error
		std::string    name;     // event name ("" for the other kinds)
		std::string    mod;      // state mod
		std::string    key;      // state key
		std::string    id;       // reply/error correlation id
		nlohmann::json payload;  // event/reply/error payload, or a state's VALUE
	};

	std::vector<Sent> g_sent;

	struct ReportedProtocolFault
	{
		std::string view;
		std::string code;
	};

	std::vector<ReportedProtocolFault> g_protocolFaults;

	std::vector<Sent> EventsTo(std::string_view a_view, std::string_view a_name)
	{
		std::vector<Sent> out;
		for (const auto& s : g_sent) {
			if (s.view == a_view && s.kind == "event" && s.name == a_name) {
				out.push_back(s);
			}
		}
		return out;
	}

	std::vector<Sent> StateTo(std::string_view a_view, std::string_view a_mod, std::string_view a_key)
	{
		std::vector<Sent> out;
		for (const auto& s : g_sent) {
			if (s.view == a_view && s.kind == "state" && s.mod == a_mod && s.key == a_key) {
				out.push_back(s);
			}
		}
		return out;
	}

	// Envelopes of one kind (ready / reply / error) for one view.
	std::vector<Sent> KindTo(std::string_view a_view, std::string_view a_kind)
	{
		std::vector<Sent> out;
		for (const auto& s : g_sent) {
			if (s.view == a_view && s.kind == a_kind) {
				out.push_back(s);
			}
		}
		return out;
	}

	void Send(OSFUI::MessageBridge& a_bridge, std::string_view a_view, std::string_view a_name,
		nlohmann::json a_payload = nlohmann::json::object())
	{
		const nlohmann::json envelope = {
			{ "kind", "send" },
			{ "name", std::string(a_name) },
			{ "payload", std::move(a_payload) },
		};
		a_bridge.HandleWebMessage(a_view, envelope.dump());
	}

	void Request(OSFUI::MessageBridge& a_bridge, std::string_view a_view, std::string_view a_name,
		std::string_view a_id, nlohmann::json a_payload = nlohmann::json::object())
	{
		const nlohmann::json envelope = {
			{ "kind", "request" },
			{ "name", std::string(a_name) },
			{ "id", std::string(a_id) },
			{ "payload", std::move(a_payload) },
		};
		a_bridge.HandleWebMessage(a_view, envelope.dump());
	}

	void Greet(OSFUI::MessageBridge& a_bridge, std::string_view a_view)
	{
		a_bridge.OnViewCreated(a_view);
		Send(a_bridge, a_view, "osfui.hello");
	}
}

// Core/Log.h declarations (real impl pulls game deps — stub).
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

	std::vector<std::string> coreHeard;  // the native reaction (change listener #0)
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
		CHECK(!msg.is_discarded());
		const auto kind = msg.value("kind", "");
		g_sent.push_back(Sent{
			.view = std::string(a_view),
			.kind = kind,
			.name = msg.value("name", ""),
			.mod = msg.value("mod", ""),
			.key = msg.value("key", ""),
			.id = msg.value("id", ""),
			.payload = kind == "state" ? msg.value("value", nlohmann::json()) :
										 msg.value("payload", nlohmann::json()),
		});
	});
	bridge.SetProtocolFaultSink([](std::string_view a_view, std::string_view a_code, std::string_view,
							const nlohmann::json&, bool) {
		g_protocolFaults.push_back(ReportedProtocolFault{ std::string(a_view), std::string(a_code) });
	});
	bridge.SetHelloHook([&](std::string_view a_view) {
		bridge.PublishState(a_view, "osfui", "settings", module.Store().DataView());
	});
	module.RegisterEndpoints(bridge);

	g_sent.clear();
	Request(bridge, "osfui/settings", "settings.get", "removed-get");
	CHECK(KindTo("osfui/settings", "error").size() == 1);
	CHECK(KindTo("osfui/settings", "error")[0].payload.value("code", "") == "unknown-endpoint");

	// --- boot: the registry arrives as STATE, answering a greeting -----------
	g_sent.clear();
	Greet(bridge, "osfui/settings");
	Greet(bridge, "t.alpha/hud");
	bridge.OnViewCreated("t.alpha/other");
	{
		CHECK(StateTo("osfui/settings", "osfui", "settings").size() == 1);
		CHECK(StateTo("t.alpha/hud", "osfui", "settings").size() == 1);
		CHECK(StateTo("t.alpha/other", "osfui", "settings").empty());
		CHECK(StateTo("osfui/settings", "osfui", "settings")[0].payload["mods"].size() == 1);

		CHECK(g_sent.size() == 4);
		CHECK(g_sent[0].view == "osfui/settings" && g_sent[0].kind == "ready");
		CHECK(g_sent[1].view == "osfui/settings" && g_sent[1].kind == "state");
		CHECK(g_sent[0].payload["view"] == "osfui/settings" && g_sent[0].payload["mod"] == "osfui");
	}

	// --- settings.set: reply to the caller, settings.changed to every view ---
	g_sent.clear();
	Request(bridge, "osfui/settings", "settings.set", "r-scale",
		{ { "mod", "t.alpha" }, { "key", "scale" }, { "value", 1.5 } });
	{
		const auto replies = KindTo("osfui/settings", "reply");
		CHECK(replies.size() == 1 && replies[0].id == "r-scale");
		CHECK(replies.size() == 1 && replies[0].payload["mod"] == "t.alpha" &&
		      replies[0].payload["key"] == "scale" && replies[0].payload["value"] == 1.5);
		CHECK(KindTo("osfui/settings", "error").empty());
		CHECK(KindTo("t.alpha/hud", "reply").empty());  // a reply is caller-only

		const auto toSettings = EventsTo("osfui/settings", "settings.changed");
		const auto toHud = EventsTo("t.alpha/hud", "settings.changed");
		CHECK(toSettings.size() == 1);
		CHECK(toHud.size() == 1);
		CHECK(toHud[0].payload["mod"] == "t.alpha" && toHud[0].payload["key"] == "scale" && toHud[0].payload["value"] == 1.5);
		CHECK(EventsTo("t.alpha/other", "settings.changed").empty());

		CHECK(EventsTo("osfui/settings", "settings.persisted").empty());
	}

	// --- write-behind flush lands: settings.persisted to every greeted view ---
	g_sent.clear();
	module.Store().FlushPersistence();  // the set above left alpha dirty
	for (const auto* view : { "osfui/settings", "t.alpha/hud" }) {
		const auto persisted = EventsTo(view, "settings.persisted");
		CHECK(persisted.size() == 1 && persisted[0].payload["mod"] == "t.alpha");
	}
	g_sent.clear();
	module.Store().FlushPersistence();  // nothing dirty — no push
	CHECK(g_sent.empty());

	g_sent.clear();
	Request(bridge, "osfui/settings", "settings.set", "r-bad",
		{ { "mod", "t.alpha" }, { "key", "scale" }, { "value", "huge" } });
	{
		const auto errors = KindTo("osfui/settings", "error");
		CHECK(errors.size() == 1 && errors[0].id == "r-bad");
		CHECK(errors.size() == 1 && errors[0].payload["code"] == "invalid-value");
		CHECK(KindTo("osfui/settings", "reply").empty());
		CHECK(EventsTo("t.alpha/hud", "settings.changed").empty());
	}

	g_sent.clear();
	Request(bridge, "t.alpha/other", "settings.set", "r-other",
		{ { "mod", "t.alpha" }, { "key", "enabled" }, { "value", false } });
	CHECK(KindTo("t.alpha/other", "reply").size() == 1);
	CHECK(EventsTo("t.alpha/other", "settings.changed").empty());  // ungreeted: no pushes
	CHECK(EventsTo("t.alpha/hud", "settings.changed").size() == 1);

	{
		// t.keys/panel names t.alpha: refused, nothing pushed, value intact.
		g_sent.clear();
		const auto before = module.Store().GetValue("t.alpha", "scale");
		CHECK(before != nullptr);
		const auto keep = *before;
		Request(bridge, "t.keys/panel", "settings.set", "r-forbidden",
			{ { "mod", "t.alpha" }, { "key", "scale" }, { "value", 9.0 } });
		{
			const auto errors = KindTo("t.keys/panel", "error");
			CHECK(errors.size() == 1 && errors[0].payload["code"] == "forbidden");
			CHECK(KindTo("t.keys/panel", "reply").empty());
			CHECK(EventsTo("t.alpha/hud", "settings.changed").empty());
			CHECK(*module.Store().GetValue("t.alpha", "scale") == keep);
		}

		// A cross-mod reset is a write too.
		g_sent.clear();
		Request(bridge, "t.keys/panel", "settings.reset", "r-forbidden-reset",
			{ { "mod", "t.alpha" }, { "key", "" } });
		{
			const auto errors = KindTo("t.keys/panel", "error");
			CHECK(errors.size() == 1 && errors[0].payload["code"] == "forbidden");
			CHECK(StateTo("t.alpha/hud", "osfui", "settings").empty());  // nothing republished
		}

		g_sent.clear();
		Request(bridge, "t.alpha/other", "settings.set", "r-implicit-mod",
			{ { "key", "scale" }, { "value", 1.25 } });
		{
			const auto replies = KindTo("t.alpha/other", "reply");
			CHECK(replies.size() == 1 && replies[0].payload["mod"] == "t.alpha");
			CHECK(replies.size() == 1 && replies[0].payload["value"] == 1.25);
		}

	}

	{
		g_sent.clear();
		g_protocolFaults.clear();
		const auto keep = *module.Store().GetValue("t.alpha", "scale");
		Send(bridge, "osfui/settings", "settings.set",
			{ { "mod", "t.alpha" }, { "key", "scale" }, { "value", 0.75 } });
		CHECK(g_protocolFaults.size() == 1 && g_protocolFaults[0].code == "wrong-endpoint-kind");
		CHECK(g_protocolFaults.size() == 1 && g_protocolFaults[0].view == "osfui/settings");
		CHECK(*module.Store().GetValue("t.alpha", "scale") == keep);  // not committed
		CHECK(g_sent.empty());                                        // and nothing settled
	}

	// --- settings.reset: ONE state republish, no per-key event spam ----------
	g_sent.clear();
	Request(bridge, "osfui/settings", "settings.reset", "r-reset",
		{ { "mod", "t.alpha" }, { "key", "" } });
	{
		const auto replies = KindTo("osfui/settings", "reply");
		CHECK(replies.size() == 1 && replies[0].id == "r-reset" && replies[0].payload.empty());
		CHECK(StateTo("osfui/settings", "osfui", "settings").size() == 1);
		CHECK(StateTo("t.alpha/hud", "osfui", "settings").size() == 1);
		CHECK(EventsTo("osfui/settings", "settings.changed").empty());  // superseded by the republish
		CHECK(EventsTo("t.alpha/hud", "settings.changed").empty());
	}

	g_sent.clear();
	Request(bridge, "t.alpha/other", "settings.reset", "r-reset-other",
		{ { "mod", "t.alpha" }, { "key", "" } });
	CHECK(KindTo("t.alpha/other", "reply").size() == 1);
	CHECK(StateTo("t.alpha/other", "osfui", "settings").empty());
	CHECK(StateTo("t.alpha/hud", "osfui", "settings").size() == 1);

	// --- new drop-in: value replay + registry republish -----------------------
	g_sent.clear();
	WriteFile(schemaDir / "t.gamma.json", R"json({
		"id": "t.gamma", "title": "Gamma",
		"groups": [ { "label": "G", "settings": [
			{ "key": "level", "type": "int", "default": 1, "min": 0, "max": 10 }
		] } ] })json");
	CHECK(module.Store().ReloadDropInFile(schemaDir / "t.gamma.json"));
	{
		// Value replay reaches greeted views as settings.changed events...
		const auto changed = EventsTo("t.alpha/hud", "settings.changed");
		CHECK(changed.size() == 1 && changed[0].payload["mod"] == "t.gamma");
		// ...and the SHAPE change republishes the whole registry to BOTH.
		for (const auto* view : { "osfui/settings", "t.alpha/hud" }) {
			const auto data = StateTo(view, "osfui", "settings");
			CHECK(data.size() == 1 && data[0].payload["mods"].size() == 2);
		}
		CHECK(StateTo("t.alpha/other", "osfui", "settings").empty());  // never greeted
	}

	// --- removal republishes too ---------------------------------------------
	g_sent.clear();
	CHECK(module.Store().RemoveMod("t.gamma"));
	fs::remove(schemaDir / "t.gamma.json");
	{
		const auto data = StateTo("t.alpha/hud", "osfui", "settings");
		CHECK(data.size() == 1 && data[0].payload["mods"].size() == 1);
	}

	// --- PushHotkey: ui.hotkey to every greeted view (views filter on mod) ----
	g_sent.clear();
	module.PushHotkey("t.alpha", "toggleHud");
	for (const auto* view : { "osfui/settings", "t.alpha/hud" }) {
		const auto hotkeys = EventsTo(view, "ui.hotkey");
		CHECK(hotkeys.size() == 1 && hotkeys[0].payload["mod"] == "t.alpha" && hotkeys[0].payload["key"] == "toggleHud");
	}
	CHECK(EventsTo("t.alpha/other", "ui.hotkey").empty());  // never greeted

	g_sent.clear();
	bridge.OnViewDestroyed("t.alpha/hud");
	CHECK(module.Store().Set("t.alpha", "scale", "0.75"));
	CHECK(EventsTo("t.alpha/hud", "settings.changed").empty());
	CHECK(EventsTo("osfui/settings", "settings.changed").size() == 1);  // others unaffected

	// --- OnBridgeDown: pushes stop, nothing dangles ---------------------------
	g_sent.clear();
	module.OnBridgeDown();
	CHECK(module.Store().Set("t.alpha", "scale", "0.5"));  // direct native write (the ABI path)
	CHECK(g_sent.empty());

	module.RegisterEndpoints(bridge);
	CHECK(module.Store().Set("t.alpha", "scale", "1.25"));
	CHECK(EventsTo("osfui/settings", "settings.changed").size() == 1);
	CHECK(EventsTo("t.alpha/hud", "settings.changed").empty());  // destroyed above

	// --- schema hot-reload ---------------------------------------------------
	{
		g_sent.clear();
		Send(bridge, "osfui/settings", "osfui.hello");
		{
			const auto replayed = StateTo("osfui/settings", "osfui", "settings");
			CHECK(replayed.size() == 1 && replayed[0].payload["mods"][0]["values"]["scale"] == 1.25);
		}

		module.PumpSchemaHotReload(10.0);

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

		// Past the window: reloaded — the new registry is published, values preserved.
		module.PumpSchemaHotReload(11.0);
		{
			const auto data = StateTo("osfui/settings", "osfui", "settings");
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
			const auto data = StateTo("osfui/settings", "osfui", "settings");
			CHECK(!data.empty() && data.back().payload["mods"].size() == 2);
		}

		// Deleting the file drops the mod and republishes.
		fs::remove(schemaDir / "t.delta.json");
		g_sent.clear();
		module.PumpSchemaHotReload(13.0);
		{
			const auto data = StateTo("osfui/settings", "osfui", "settings");
			CHECK(data.size() == 1 && data[0].payload["mods"].size() == 1);
		}
	}

	// --- settlement shape: post-clamp value, id echo, machine failure codes ---
	{
		g_sent.clear();
		Request(bridge, "osfui/settings", "settings.set", "q1",
			{ { "mod", "t.alpha" }, { "key", "scale" }, { "value", 1.75 } });
		{
			const auto replies = KindTo("osfui/settings", "reply");
			CHECK(replies.size() == 1 && replies[0].payload["value"] == 1.75);
			CHECK(replies.size() == 1 && replies[0].id == "q1");
			CHECK(KindTo("osfui/settings", "error").empty());
		}
		g_sent.clear();
		Request(bridge, "osfui/settings", "settings.set", "q2",
			{ { "mod", "t.alpha" }, { "key", "scale" }, { "value", 99.0 } });
		{
			const auto replies = KindTo("osfui/settings", "reply");
			CHECK(replies.size() == 1 && replies[0].payload["value"] == 2.0);
			CHECK(replies.size() == 1 && replies[0].id == "q2");
		}
		// Failures carry the machine code on the error payload.
		const auto rejectionCode = [&](std::string_view a_id, nlohmann::json a_payload) {
			g_sent.clear();
			Request(bridge, "osfui/settings", "settings.set", a_id, std::move(a_payload));
			const auto errors = KindTo("osfui/settings", "error");
			CHECK(errors.size() == 1 && errors[0].id == a_id);
			CHECK(KindTo("osfui/settings", "reply").empty());
			return errors.empty() ? std::string{} : errors[0].payload.value("code", "");
		};
		CHECK(rejectionCode("q3", { { "mod", "t.alpha" }, { "key", "scale" }, { "value", "huge" } }) == "invalid-value");
		CHECK(rejectionCode("q4", { { "mod", "t.alpha" }, { "key", "nope" }, { "value", 1 } }) == "unknown-setting");
		CHECK(rejectionCode("q5", { { "mod", "t.alpha" }, { "key", "scale" } }) == "invalid-value");  // no value field

		// A failed reset rejects and republishes nothing.
		g_sent.clear();
		Request(bridge, "osfui/settings", "settings.reset", "q6", { { "mod", "nope.mod" }, { "key", "" } });
		{
			const auto errors = KindTo("osfui/settings", "error");
			CHECK(errors.size() == 1 && errors[0].payload["code"] == "unknown-setting" && errors[0].id == "q6");
			CHECK(StateTo("osfui/settings", "osfui", "settings").empty());
		}
	}

	// --- key-typed settings.changed carries recomputed conflicts -------------
	{
		module.Store().SetKeyNameResolver([](std::string_view a_name) -> std::uint32_t {
			if (a_name == "F6") return 0x75;
			if (a_name == "F7") return 0x76;
			return 0;
		});
		WriteFile(schemaDir / "t.keys.json", R"json({
			"id": "t.keys", "title": "Keys",
			"groups": [ { "settings": [
				{ "key": "one", "type": "key", "default": "F6" },
				{ "key": "two", "type": "key", "default": "F7" }
			] } ] })json");
		CHECK(module.Store().ReloadDropInFile(schemaDir / "t.keys.json"));

		// Rebind INTO a collision: the event names the partner.
		g_sent.clear();
		Request(bridge, "osfui/settings", "settings.set", "k1",
			{ { "mod", "t.keys" }, { "key", "one" }, { "value", "F7" } });
		{
			const auto changed = EventsTo("osfui/settings", "settings.changed");
			CHECK(changed.size() == 1 && changed[0].payload.contains("conflicts"));
			CHECK(changed.size() == 1 && changed[0].payload["conflicts"].size() == 1 &&
			      changed[0].payload["conflicts"][0]["mod"] == "t.keys" &&
			      changed[0].payload["conflicts"][0]["key"] == "two");
		}
		// Rebind OUT again: conflicts present but EMPTY (the badge-clearing signal).
		g_sent.clear();
		Request(bridge, "osfui/settings", "settings.set", "k2",
			{ { "mod", "t.keys" }, { "key", "one" }, { "value", "F6" } });
		{
			const auto changed = EventsTo("osfui/settings", "settings.changed");
			CHECK(changed.size() == 1 && changed[0].payload.contains("conflicts"));
			CHECK(changed.size() == 1 && changed[0].payload["conflicts"].empty());
		}
		// Non-key settings never carry the field.
		g_sent.clear();
		Request(bridge, "osfui/settings", "settings.set", "k3",
			{ { "mod", "t.alpha" }, { "key", "enabled" }, { "value", true } });
		{
			const auto changed = EventsTo("osfui/settings", "settings.changed");
			CHECK(changed.size() == 1 && !changed[0].payload.contains("conflicts"));
		}
	}

	g_sent.clear();
	Send(bridge, "t.alpha/other", "osfui.hello");
	{
		CHECK(KindTo("t.alpha/other", "ready").size() == 1);
		const auto replayed = StateTo("t.alpha/other", "osfui", "settings");
		CHECK(replayed.size() == 1);
		CHECK(replayed.size() == 1 && replayed[0].payload["mods"].size() == 2);  // alpha, keys
	}

	// --- failed scan is not an authoritative empty directory -----------------
	{
		const auto scanRoot = root / "scan-failure";
		const auto scanSchemas = scanRoot / "settings";
		const auto scanValues = scanRoot / "values";
		const auto unavailable = scanRoot / "settings-unavailable";
		WriteFile(scanSchemas / "t.scan.json", R"json({
			"id": "t.scan", "groups": [ { "settings": [
				{ "key": "enabled", "type": "bool", "default": true }
			] } ] })json");

		SettingsModule guarded(scanSchemas, scanValues,
			[](std::string_view, std::string_view, const nlohmann::json&) {});
		CHECK(guarded.Store().GetValue("t.scan", "enabled") != nullptr);
		const auto generation = guarded.Store().Generation();

		fs::rename(scanSchemas, unavailable);
		guarded.PumpSchemaHotReload(1.0);
		CHECK(guarded.Store().GetValue("t.scan", "enabled") != nullptr);
		CHECK(guarded.Store().Generation() == generation);

		fs::create_directories(scanSchemas);
		guarded.PumpSchemaHotReload(2.0);
		CHECK(guarded.Store().GetValue("t.scan", "enabled") == nullptr);
		CHECK(guarded.Store().Generation() > generation);
	}

	module.OnBridgeDown();

	// ---------------------------------------------------------------------------
	std::fprintf(stderr, "%d/%d checks passed\n", g_checks - g_failures, g_checks);
	fs::remove_all(root);
	return g_failures;
}
