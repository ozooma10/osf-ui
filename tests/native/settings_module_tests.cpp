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
	// envelope from a source view.
	void Command(OSFUI::MessageBridge& a_bridge, std::string_view a_view, nlohmann::json a_payload)
	{
		const nlohmann::json envelope = { { "type", "ui.command" }, { "payload", std::move(a_payload) } };
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

	WriteFile(schemaDir / "alpha.json", R"json({
		"id": "alpha", "title": "Alpha Mod",
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
		g_sent.push_back({ std::string(a_view), msg.value("type", ""), msg.value("payload", nlohmann::json()) });
	});
	module.RegisterCommands(bridge);

	// --- subscribe-on-read -----------------------------------------------------
	Command(bridge, "settingsview", { { "command", "settings.get" } });
	Command(bridge, "hudview", { { "command", "settings.get" } });
	CHECK(SentTo("settingsview", "settings.data").size() == 1);
	CHECK(SentTo("hudview", "settings.data").size() == 1);

	// --- settings.set: ack to caller, settings.changed to ALL subscribers ------
	g_sent.clear();
	Command(bridge, "settingsview", { { "command", "settings.set" }, { "mod", "alpha" }, { "key", "scale" }, { "value", 1.5 } });
	{
		const auto acks = SentTo("settingsview", "settings.ack");
		CHECK(acks.size() == 1 && acks[0].payload["ok"] == true);
		CHECK(SentTo("hudview", "settings.ack").empty());  // ack is caller-only

		const auto toSettings = SentTo("settingsview", "settings.changed");
		const auto toHud = SentTo("hudview", "settings.changed");
		CHECK(toSettings.size() == 1);
		CHECK(toHud.size() == 1);
		CHECK(toHud[0].payload["mod"] == "alpha" && toHud[0].payload["key"] == "scale" && toHud[0].payload["value"] == 1.5);

		// Write-behind: the commit pushed settings.changed immediately, but the
		// disk write (and its settings.persisted confirmation) waits for the flush.
		CHECK(SentTo("settingsview", "settings.persisted").empty());
	}

	// --- write-behind flush lands: settings.persisted to ALL subscribers ---------
	g_sent.clear();
	module.Store().FlushPersistence();  // the set above left alpha dirty
	for (const auto* view : { "settingsview", "hudview" }) {
		const auto persisted = SentTo(view, "settings.persisted");
		CHECK(persisted.size() == 1 && persisted[0].payload["mod"] == "alpha");
	}
	g_sent.clear();
	module.Store().FlushPersistence();  // nothing dirty — no push
	CHECK(g_sent.empty());

	// --- rejected set: ack ok:false, NO settings.changed ------------------------
	g_sent.clear();
	Command(bridge, "settingsview", { { "command", "settings.set" }, { "mod", "alpha" }, { "key", "scale" }, { "value", "huge" } });
	{
		const auto acks = SentTo("settingsview", "settings.ack");
		CHECK(acks.size() == 1 && acks[0].payload["ok"] == false);
		CHECK(SentTo("hudview", "settings.changed").empty());
	}

	// --- a non-subscriber can set (it never called settings.get) ----------------
	g_sent.clear();
	Command(bridge, "otherview", { { "command", "settings.set" }, { "mod", "alpha" }, { "key", "enabled" }, { "value", false } });
	CHECK(SentTo("otherview", "settings.ack").size() == 1);
	CHECK(SentTo("otherview", "settings.changed").empty());  // not subscribed
	CHECK(SentTo("hudview", "settings.changed").size() == 1);

	// --- settings.reset: ONE settings.data to every subscriber, no per-key spam --
	g_sent.clear();
	Command(bridge, "settingsview", { { "command", "settings.reset" }, { "mod", "alpha" }, { "key", "" } });
	CHECK(SentTo("settingsview", "settings.data").size() == 1);
	CHECK(SentTo("hudview", "settings.data").size() == 1);
	CHECK(SentTo("settingsview", "settings.changed").empty());  // superseded by the data re-send
	CHECK(SentTo("hudview", "settings.changed").empty());

	// A caller that never subscribed still gets the authoritative re-send.
	g_sent.clear();
	Command(bridge, "otherview", { { "command", "settings.reset" }, { "mod", "alpha" }, { "key", "" } });
	CHECK(SentTo("otherview", "settings.data").size() == 1);
	CHECK(SentTo("hudview", "settings.data").size() == 1);

	// --- runtime registration: replay + settings.data re-broadcast (§8.5) -------
	g_sent.clear();
	auto gamma = nlohmann::json::parse(R"json({
		"id": "gamma", "title": "Gamma (runtime)",
		"groups": [ { "label": "G", "settings": [
			{ "key": "level", "type": "int", "default": 1, "min": 0, "max": 10 }
		] } ] })json");
	CHECK(module.Store().RegisterSchema(gamma, SettingsStore::Source::kNative));
	{
		// Value replay reaches subscribers as settings.changed...
		const auto changed = SentTo("hudview", "settings.changed");
		CHECK(changed.size() == 1 && changed[0].payload["mod"] == "gamma");
		// ...and the shape change re-broadcasts the full registry to BOTH.
		for (const auto* view : { "settingsview", "hudview" }) {
			const auto data = SentTo(view, "settings.data");
			CHECK(data.size() == 1 && data[0].payload["mods"].size() == 2);
		}
		CHECK(SentTo("otherview", "settings.data").empty());  // never subscribed
	}

	// --- removal re-broadcasts too ----------------------------------------------
	g_sent.clear();
	CHECK(module.Store().RemoveMod("gamma"));
	{
		const auto data = SentTo("hudview", "settings.data");
		CHECK(data.size() == 1 && data[0].payload["mods"].size() == 1);
	}

	// --- OnViewDestroyed: a torn-down view stops receiving pushes -----------------
	g_sent.clear();
	module.OnViewDestroyed("hudview");
	CHECK(module.Store().Set("alpha", "scale", "0.75"));
	CHECK(SentTo("hudview", "settings.changed").empty());
	CHECK(SentTo("settingsview", "settings.changed").size() == 1);  // others unaffected

	// --- OnBridgeDown: pushes stop, nothing dangles -------------------------------
	g_sent.clear();
	module.OnBridgeDown();
	CHECK(module.Store().Set("alpha", "scale", "0.5"));  // direct native write (future ABI path)
	CHECK(g_sent.empty());

	// A fresh RegisterCommands starts a clean subscriber set (views reload and
	// re-subscribe; stale ids must not receive pushes).
	module.RegisterCommands(bridge);
	CHECK(module.Store().Set("alpha", "scale", "1.25"));
	CHECK(g_sent.empty());

	// ---------------------------------------------------------------------------
	std::fprintf(stderr, "%d/%d checks passed\n", g_checks - g_failures, g_checks);
	fs::remove_all(root);
	return g_failures;
}
