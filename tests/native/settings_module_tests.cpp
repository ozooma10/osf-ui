// Host-side integration tests for the settings web path (protocol 2.0,
// docs/mod-api-2.0-design.md): the REAL SettingsModule + MessageBridge +
// SettingsStore driven through actual 2.0 envelopes, with a capturing SendFn
// standing in for the renderer.
//
// What moved from 1.x, and why the assertions look different:
//   * `settings.get` is GONE. It was a read whose real job was to subscribe the
//     caller, so the registry is now the `osfui/settings` STATE key — published
//     to every view that has GREETED the bridge, and replayed to each fresh
//     document by the host's hello hook (Runtime::OnViewGreeted). Every test
//     below therefore boots its views the way the host does: OnViewCreated,
//     then a page-initiated `osfui.hello`.
//   * The module's `_subscribers` set is gone with it. Delivery scope is the
//     bridge's greeted-view set, which is why an ungreeted view is the 2.0
//     stand-in for 1.x's "never subscribed".
//   * `settings.ack` is gone: settings.set REPLIES { mod, key, value } and
//     REJECTS with an error envelope, so a caller that ignores the outcome can
//     no longer mistake a refusal for success.
// Assert-style; process exit code is the failure count.

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

	// One captured native->web envelope, flattened. 2.0 keeps every routing
	// field BESIDE the payload, so they are all top-level here too — a test can
	// never accidentally assert on a payload field that shadowed routing.
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

	// Host-detected protocol misuse (MessageBridge::SetSurfaceFn). 2.0 routes it
	// out of band instead of dropping the message silently the way 1.x did.
	struct Misuse
	{
		std::string view;
		std::string code;
	};

	std::vector<Misuse> g_misuse;

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

	// State envelopes for one "<mod>/<key>" pair. Both halves are asserted: a
	// value published under the wrong mod would reach a page's `<mod>/<key>`
	// subscription and nowhere else.
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

	// --- web -> native, exactly as the page helper builds it -----------------
	// A `send` is a pure notification and carries NO id (one supplied is a hard
	// invalid-request, because the caller would be waiting for a settlement
	// that never comes).
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

	// A `request` REQUIRES a correlation id — spelling it at every call site is
	// the point: 1.x silently demoted a missing/oversized id to fire-and-forget,
	// which turned a client bug into a promise that never settled.
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

	// Boot a view the way the host does: arm its (closed) event gate, then let
	// the DOCUMENT greet. The page-initiated handshake is the only boot path,
	// so first open, F5 and crash-recovery reload are all this same sequence.
	void Greet(OSFUI::MessageBridge& a_bridge, std::string_view a_view)
	{
		a_bridge.OnViewCreated(a_view);
		Send(a_bridge, a_view, "osfui.hello");
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
		// Every envelope the bridge encodes must be valid JSON: the encoders
		// splice pre-dumped text into hand-written wrappers, and a malformed one
		// would reach the renderer, not an exception handler.
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
	bridge.SetSurfaceFn([](std::string_view a_view, std::string_view a_code, std::string_view,
							const nlohmann::json&) {
		g_misuse.push_back(Misuse{ std::string(a_view), std::string(a_code) });
	});
	// The host's whole hello obligation for this key (Runtime::OnViewGreeted):
	// publish the CURRENT registry straight to the greeting document. It does
	// not route through any change-dedupe — a dedupe against the last change
	// would send the second document to connect nothing at all.
	bridge.SetHelloHook([&](std::string_view a_view) {
		bridge.PublishState(a_view, "osfui", "settings", module.Store().DataView());
	});
	module.RegisterEndpoints(bridge);

	// The registry endpoints are exactly two REQUESTS. `settings.get` is gone
	// as a name, not merely unused: a stale 1.x view naming it must get
	// `unknown-endpoint`, never a half-working read.
	CHECK(bridge.HasRequest("settings.set"));
	CHECK(bridge.HasRequest("settings.reset"));
	CHECK(!bridge.HasRequest("settings.get") && !bridge.HasSend("settings.get"));

	// --- boot: the registry arrives as STATE, answering a greeting -----------
	g_sent.clear();
	Greet(bridge, "osfui/settings");
	Greet(bridge, "t.alpha/hud");
	// Created but never greeted — the 2.0 stand-in for 1.x's "never subscribed".
	// It stays quiet for the whole session until the last section boots it.
	bridge.OnViewCreated("t.alpha/other");
	{
		CHECK(StateTo("osfui/settings", "osfui", "settings").size() == 1);
		CHECK(StateTo("t.alpha/hud", "osfui", "settings").size() == 1);
		CHECK(StateTo("t.alpha/other", "osfui", "settings").empty());
		CHECK(StateTo("osfui/settings", "osfui", "settings")[0].payload["mods"].size() == 1);

		// Ordering is structural, not a convention call sites remember: `ready`
		// precedes every state value for a document, and state precedes the
		// first event it can see.
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
		// An event is a one-shot happening, so an ungreeted document is not a
		// target at all: it could not have been present for it, and the state it
		// actually needs arrives on its greeting.
		CHECK(EventsTo("t.alpha/other", "settings.changed").empty());

		// Write-behind: the commit pushed settings.changed immediately, but the
		// disk write (and its settings.persisted confirmation) waits for the flush.
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

	// --- rejected set: an ERROR envelope, no settings.changed ----------------
	// 1.x resolved a `settings.ack { ok:false }` the caller had to remember to
	// inspect, so forgetting read as success. A rejection cannot be ignored.
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

	// --- a view that never greeted can still write ---------------------------
	// Writing is a request, and a request settles straight back to its source:
	// it needs no gate, because nothing about it is a push.
	g_sent.clear();
	Request(bridge, "t.alpha/other", "settings.set", "r-other",
		{ { "mod", "t.alpha" }, { "key", "enabled" }, { "value", false } });
	CHECK(KindTo("t.alpha/other", "reply").size() == 1);
	CHECK(EventsTo("t.alpha/other", "settings.changed").empty());  // ungreeted: no pushes
	CHECK(EventsTo("t.alpha/hud", "settings.changed").size() == 1);

	// --- write authority (Ids::ResolveWritableMod) ---------------------------
	// A view may only write its OWN mod's settings; naming a foreign mod is
	// refused with `forbidden` and commits nothing. Only the built-in Mods
	// surface and keybinds board may write cross-mod (their entire purpose).
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

		// An omitted mod field resolves to the caller's own mod — the field
		// carries no authority for a non-editor view either way.
		g_sent.clear();
		Request(bridge, "t.alpha/other", "settings.set", "r-implicit-mod",
			{ { "key", "scale" }, { "value", 1.25 } });
		{
			const auto replies = KindTo("t.alpha/other", "reply");
			CHECK(replies.size() == 1 && replies[0].payload["mod"] == "t.alpha");
			CHECK(replies.size() == 1 && replies[0].payload["value"] == 1.25);
		}

		// The built-in Mods surface stays cross-mod capable (asserted throughout
		// this file: every osfui/settings write above targets t.alpha/t.keys).
	}

	// --- kind enforcement: a mutation sent as a `send` executes nothing -------
	// The kind is what callers dispatch on, so a request endpoint reached with
	// send() is dropped rather than run — and, unlike 1.x, the drop is SURFACED
	// to the offending view instead of vanishing.
	{
		g_sent.clear();
		g_misuse.clear();
		const auto keep = *module.Store().GetValue("t.alpha", "scale");
		Send(bridge, "osfui/settings", "settings.set",
			{ { "mod", "t.alpha" }, { "key", "scale" }, { "value", 0.75 } });
		CHECK(g_misuse.size() == 1 && g_misuse[0].code == "wrong-endpoint-kind");
		CHECK(g_misuse.size() == 1 && g_misuse[0].view == "osfui/settings");
		CHECK(*module.Store().GetValue("t.alpha", "scale") == keep);  // not committed
		CHECK(g_sent.empty());                                        // and nothing settled
	}

	// --- settings.reset: ONE state republish, no per-key event spam ----------
	g_sent.clear();
	Request(bridge, "osfui/settings", "settings.reset", "r-reset",
		{ { "mod", "t.alpha" }, { "key", "" } });
	{
		const auto replies = KindTo("osfui/settings", "reply");
		// The reply says only "the reset happened": carrying the document in it
		// as well would make the caller's copy arrive by a different route than
		// everyone else's.
		CHECK(replies.size() == 1 && replies[0].id == "r-reset" && replies[0].payload.empty());
		CHECK(StateTo("osfui/settings", "osfui", "settings").size() == 1);
		CHECK(StateTo("t.alpha/hud", "osfui", "settings").size() == 1);
		CHECK(EventsTo("osfui/settings", "settings.changed").empty());  // superseded by the republish
		CHECK(EventsTo("t.alpha/hud", "settings.changed").empty());
	}

	// A caller that has not greeted gets its reply and nothing else: state is
	// never pushed at an ungreeted document, because its greeting replays
	// everything current in one shot.
	g_sent.clear();
	Request(bridge, "t.alpha/other", "settings.reset", "r-reset-other",
		{ { "mod", "t.alpha" }, { "key", "" } });
	CHECK(KindTo("t.alpha/other", "reply").size() == 1);
	CHECK(StateTo("t.alpha/other", "osfui", "settings").empty());
	CHECK(StateTo("t.alpha/hud", "osfui", "settings").size() == 1);

	// --- runtime registration: value replay + registry republish -------------
	g_sent.clear();
	auto gamma = nlohmann::json::parse(R"json({
		"id": "t.gamma", "title": "Gamma (runtime)",
		"groups": [ { "label": "G", "settings": [
			{ "key": "level", "type": "int", "default": 1, "min": 0, "max": 10 }
		] } ] })json");
	CHECK(module.Store().RegisterSchema(gamma, SettingsStore::Source::kNative));
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

	// --- a torn-down view stops receiving pushes -----------------------------
	// The module has nothing to prune any more (its OnViewDestroyed is a no-op):
	// the gate the bridge drops IS the subscription. 1.x had to sweep a set
	// here, and a crash-recovered view that missed the sweep kept receiving
	// pushes for the process lifetime.
	g_sent.clear();
	bridge.OnViewDestroyed("t.alpha/hud");
	module.OnViewDestroyed("t.alpha/hud");
	CHECK(module.Store().Set("t.alpha", "scale", "0.75"));
	CHECK(EventsTo("t.alpha/hud", "settings.changed").empty());
	CHECK(EventsTo("osfui/settings", "settings.changed").size() == 1);  // others unaffected

	// --- OnBridgeDown: pushes stop, nothing dangles ---------------------------
	g_sent.clear();
	module.OnBridgeDown();
	CHECK(module.Store().Set("t.alpha", "scale", "0.5"));  // direct native write (the ABI path)
	CHECK(g_sent.empty());

	// Re-registering resumes delivery to exactly the views the BRIDGE still
	// considers greeted — there is no per-module subscriber set to rebuild or
	// forget to clear, which is the whole point of the state key.
	module.RegisterEndpoints(bridge);
	CHECK(module.Store().Set("t.alpha", "scale", "1.25"));
	CHECK(EventsTo("osfui/settings", "settings.changed").size() == 1);
	CHECK(EventsTo("t.alpha/hud", "settings.changed").empty());  // destroyed above

	// --- schema hot-reload (mcm-design §12.1) --------------------------------------
	{
		// A reload (F5, dev hot reload, crash recovery) is just another greeting:
		// the fresh document is replayed the current registry with nothing to
		// re-request. 1.x had to call settings.get again purely to re-subscribe.
		g_sent.clear();
		Send(bridge, "osfui/settings", "osfui.hello");
		{
			const auto replayed = StateTo("osfui/settings", "osfui", "settings");
			CHECK(replayed.size() == 1 && replayed[0].payload["mods"][0]["values"]["scale"] == 1.25);
		}

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

		// Deleting the file drops the mod (drop-ins only) and republishes.
		fs::remove(schemaDir / "t.delta.json");
		g_sent.clear();
		module.PumpSchemaHotReload(13.0);
		{
			const auto data = StateTo("osfui/settings", "osfui", "settings");
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

	// --- settlement shape: post-clamp value, id echo, machine failure codes ---
	{
		// The reply carries the authoritative post-clamp value, so the caller
		// can tell clamped from accepted without a re-fetch...
		g_sent.clear();
		Request(bridge, "osfui/settings", "settings.set", "q1",
			{ { "mod", "t.alpha" }, { "key", "scale" }, { "value", 1.75 } });
		{
			const auto replies = KindTo("osfui/settings", "reply");
			CHECK(replies.size() == 1 && replies[0].payload["value"] == 1.75);
			CHECK(replies.size() == 1 && replies[0].id == "q1");
			CHECK(KindTo("osfui/settings", "error").empty());
		}
		// ...including a CLAMPED commit, which is a success carrying the stored
		// value, not a refusal.
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
		CHECK(module.Store().RegisterSchema(nlohmann::json::parse(R"json({
			"id": "t.keys", "title": "Keys",
			"groups": [ { "settings": [
				{ "key": "one", "type": "key", "default": "F6" },
				{ "key": "two", "type": "key", "default": "F7" }
			] } ] })json"),
			SettingsStore::Source::kNative));

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

	// --- a late document boots: one greeting hands it everything -------------
	// t.alpha/other has been quiet since the top of the file and has therefore
	// missed every event and every republish. It needs no catch-up protocol: the
	// hello replay is authoritative by construction, which is exactly what
	// subscribe-on-read could not promise after a mid-session F5.
	g_sent.clear();
	Send(bridge, "t.alpha/other", "osfui.hello");
	{
		CHECK(KindTo("t.alpha/other", "ready").size() == 1);
		const auto replayed = StateTo("t.alpha/other", "osfui", "settings");
		CHECK(replayed.size() == 1);
		CHECK(replayed.size() == 1 && replayed[0].payload["mods"].size() == 3);  // alpha, epsilon, keys
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
