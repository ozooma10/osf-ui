// Host-side unit tests for BridgeApi (api-freeze-plan items 1 + 3): the REAL
// src/api/BridgeApi.cpp compiled against stubs/pch.h — command-shape
// enforcement ("<author>.<modname>.<name>", ABI 1.6), first-wins duplicate
// refusal, unregister-then-reregister replacement, qualified RegisterView ids,
// and the registry-apply/dispatch round trip through a real MessageBridge.
// NOTE: BridgeApi is a process singleton — sections share state, in order.

#include "api/BridgeApi.h"

#include "core/Log.h"
#include "runtime/MessageBridge.h"

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

	bool LoggedContaining(std::string_view a_level, std::string_view a_needle)
	{
		for (const auto& entry : REX::test::Entries()) {
			if (entry.starts_with(a_level) && entry.find(a_needle) != std::string::npos) {
				return true;
			}
		}
		return false;
	}

	// Recorded command-handler firings.
	struct Fired
	{
		std::string command;
		std::string payload;
		std::string source;
	};
	std::vector<Fired> g_firedA;
	std::vector<Fired> g_firedB;

	void HandlerA(const char* a_cmd, const char* a_payload, const char* a_src, void*) noexcept
	{
		g_firedA.push_back({ a_cmd, a_payload, a_src });
	}
	void HandlerB(const char* a_cmd, const char* a_payload, const char* a_src, void*) noexcept
	{
		g_firedB.push_back({ a_cmd, a_payload, a_src });
	}
}

// core/Log.h declarations (real impl pulls game deps — stub it here).
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
	using OSFUI::MessageBridge;
	auto& api = OSFUI::API::BridgeApi::Get();

	// --- version constants: 1.6 (command-shape guarantee) ---------------------
	CHECK(OSFUI::API::kBridgeAPIMajor == 1);
	CHECK(OSFUI::API::kBridgeAPIMinor == 6);
	CHECK(api.GetInterfaceVersion() == OSFUI::API::kBridgeAPIVersion);

	// --- command shape (item 3): two dots minimum, item-1 mod-id grammar ------
	// Every platform command is structurally unregisterable — dotless verbs,
	// single-dot names (including the osfui.* built-ins), bad mod ids.
	for (const auto* bad : { "close", "ping", "menu.open", "game.get", "settings.set",
	                         "views.get", "osfui.gamepadRaw", "ui.result",
	                         "Acme.Mod.x", "under_score.mod.x", "acme.mymod.",
	                         ".leading.x", "a..b.x" }) {
		api.RegisterCommand(bad, &HandlerA, nullptr);
	}
	CHECK(LoggedContaining("WARN", "refused RegisterCommand('close')"));
	CHECK(LoggedContaining("WARN", "refused RegisterCommand('menu.open')"));
	CHECK(LoggedContaining("WARN", "refused RegisterCommand('osfui.gamepadRaw')"));
	CHECK(LoggedContaining("WARN", "refused RegisterCommand('Acme.Mod.x')"));

	// Accepted: "<author>.<modname>.<name>", name may itself contain dots.
	api.RegisterCommand("acme.mymod.ping", &HandlerA, nullptr);
	api.RegisterCommand("acme.mymod.catalog.get", &HandlerA, nullptr);

	// --- duplicates: first-wins, refused (item 3) -----------------------------
	api.RegisterCommand("acme.mymod.ping", &HandlerB, nullptr);  // hijack attempt
	CHECK(LoggedContaining("WARN", "refused RegisterCommand('acme.mymod.ping') — already registered"));

	// --- apply to a live bridge + dispatch round trip -------------------------
	std::vector<std::pair<std::string, std::string>> toWeb;  // (viewId, json)
	MessageBridge bridge([&](std::string_view a_viewId, std::string_view a_json) {
		toWeb.emplace_back(std::string(a_viewId), std::string(a_json));
	});
	api.OnBridgeReady(&bridge);
	api.PumpMainThread();

	// ABI sends validate once, retain the parsed payload while queued, and
	// produce the normal bridge envelope when the main-thread pump drains.
	toWeb.clear();
	CHECK(api.SendToWeb("someview", "acme.mymod.data", R"({"x":1,"label":"ok"})"));
	CHECK(!api.SendToWeb("someview", "acme.mymod.data", "{ bad json"));
	api.PumpMainThread();
	CHECK(toWeb.size() == 1);
	if (!toWeb.empty()) {
		const auto message = nlohmann::json::parse(toWeb.back().second, nullptr, false);
		CHECK(message["type"] == "acme.mymod.data");
		CHECK(message["payload"]["x"] == 1);
		CHECK(message["payload"]["label"] == "ok");
	}

	// Multi-target pushes encode one identical envelope and retain target ids.
	toWeb.clear();
	bridge.SendToWeb(std::unordered_set<std::string>{ "view-a", "view-b" },
		"test.broadcast", nlohmann::json{ { "large", nlohmann::json::array({ 1, 2, 3 }) } });
	CHECK(toWeb.size() == 2);
	if (toWeb.size() == 2) {
		CHECK(toWeb[0].second == toWeb[1].second);
		CHECK(toWeb[0].first != toWeb[1].first);
		const auto message = nlohmann::json::parse(toWeb[0].second, nullptr, false);
		CHECK(message["type"] == "test.broadcast");
		CHECK(message["payload"]["large"] == nlohmann::json::array({ 1, 2, 3 }));
	}
	toWeb.clear();

	bridge.HandleWebMessage("someview",
		R"({ "type": "ui.command", "payload": { "command": "acme.mymod.ping", "x": 1 } })");
	CHECK(g_firedA.size() == 1);
	CHECK(g_firedB.empty());  // the duplicate registration never took
	if (!g_firedA.empty()) {
		CHECK(g_firedA[0].command == "acme.mymod.ping");
		CHECK(g_firedA[0].source == "someview");
		CHECK(g_firedA[0].payload.find("\"x\"") != std::string::npos);
	}
	bridge.HandleWebMessage("someview",
		R"({ "type": "ui.command", "payload": { "command": "acme.mymod.catalog.get" } })");
	CHECK(g_firedA.size() == 2);

	// A refused (platform-shaped) registration must not exist on the bridge:
	// the platform verb dispatches to... nothing here (no core handler in this
	// harness), and crucially NOT to HandlerA.
	bridge.HandleWebMessage("someview",
		R"({ "type": "ui.command", "payload": { "command": "close" } })");
	CHECK(g_firedA.size() == 2);

	// --- replacement is explicit: unregister, then re-register ----------------
	api.UnregisterCommand("acme.mymod.ping");
	api.RegisterCommand("acme.mymod.ping", &HandlerB, nullptr);  // now legal (slot free)
	api.PumpMainThread();
	bridge.HandleWebMessage("someview",
		R"({ "type": "ui.command", "payload": { "command": "acme.mymod.ping" } })");
	CHECK(g_firedA.size() == 2);
	CHECK(g_firedB.size() == 1);

	// --- item 5 (protocol 1.0): the request/result envelope -------------------
	{
		// A plugin command with a requestId: the payload handed to the plugin
		// carries it, and the bridge auto-acks ui.result { ok:true }.
		toWeb.clear();
		bridge.HandleWebMessage("someview",
			R"({ "type": "ui.command", "requestId": "r1", "payload": { "command": "acme.mymod.ping" } })");
		CHECK(g_firedB.size() == 2);
		CHECK(!g_firedB.empty() && g_firedB.back().payload.find("\"requestId\":\"r1\"") != std::string::npos);
		CHECK(toWeb.size() == 1);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"type\":\"ui.result\"") != std::string::npos);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"requestId\":\"r1\"") != std::string::npos);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"ok\":true") != std::string::npos);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"command\":\"acme.mymod.ping\"") != std::string::npos);

		// Fire-and-forget (no requestId): no ui.result.
		toWeb.clear();
		bridge.HandleWebMessage("someview",
			R"({ "type": "ui.command", "payload": { "command": "acme.mymod.ping" } })");
		CHECK(toWeb.empty());
		CHECK(!g_firedB.empty() && g_firedB.back().payload.find("requestId") == std::string::npos);

		// An over-long requestId (>64 chars) is ignored — treated fire-and-forget.
		const std::string longId(65, 'x');
		bridge.HandleWebMessage("someview",
			std::string(R"({ "type": "ui.command", "requestId": ")") + longId +
				R"(", "payload": { "command": "acme.mymod.ping" } })");
		CHECK(toWeb.empty());

		// A handler that replies through the no-target SendToWeb: the reply
		// itself carries the requestId and suppresses the auto ui.result.
		bridge.RegisterCommand("test.reply", [](const nlohmann::json&, MessageBridge& a_b) {
			a_b.SendToWeb("test.data", nlohmann::json{ { "v", 7 } });
		});
		toWeb.clear();
		bridge.HandleWebMessage("someview",
			R"({ "type": "ui.command", "requestId": "r2", "payload": { "command": "test.reply" } })");
		CHECK(toWeb.size() == 1);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"type\":\"test.data\"") != std::string::npos);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"requestId\":\"r2\"") != std::string::npos);

		// SendResult(false, code): the explicit failure outcome...
		bridge.RegisterCommand("test.fail", [](const nlohmann::json&, MessageBridge& a_b) {
			a_b.SendResult(false, "unknown-view", "nope");
		});
		toWeb.clear();
		bridge.HandleWebMessage("someview",
			R"({ "type": "ui.command", "requestId": "r3", "payload": { "command": "test.fail" } })");
		CHECK(toWeb.size() == 1);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"ok\":false") != std::string::npos);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"code\":\"unknown-view\"") != std::string::npos);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"requestId\":\"r3\"") != std::string::npos);
		// ...which stays SILENT for a fire-and-forget caller.
		toWeb.clear();
		bridge.HandleWebMessage("someview",
			R"({ "type": "ui.command", "payload": { "command": "test.fail" } })");
		CHECK(toWeb.empty());

		// DeferResult: no auto-ack now; the deferred reply carries the id later
		// (the settings.captureKey pattern).
		std::string deferredId;
		bridge.RegisterCommand("test.defer", [&deferredId](const nlohmann::json&, MessageBridge& a_b) {
			deferredId = std::string(a_b.CurrentRequestId());
			a_b.DeferResult();
		});
		toWeb.clear();
		bridge.HandleWebMessage("someview",
			R"({ "type": "ui.command", "requestId": "r4", "payload": { "command": "test.defer" } })");
		CHECK(toWeb.empty());
		CHECK(deferredId == "r4");
		bridge.SendToWeb("someview", "test.done", nlohmann::json::object(), deferredId);
		CHECK(toWeb.size() == 1);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"requestId\":\"r4\"") != std::string::npos);

		// ui.error shape: machine code + message + id echo. The pre-1.0
		// `reason` duplicate of message must NOT be emitted anymore.
		toWeb.clear();
		bridge.HandleWebMessage("someview",
			R"({ "type": "ui.command", "requestId": "r5", "payload": { "command": "nope" } })");
		CHECK(toWeb.size() == 1);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"type\":\"ui.error\"") != std::string::npos);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"code\":\"unknown-command\"") != std::string::npos);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"message\":") != std::string::npos);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"reason\":") == std::string::npos);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"requestId\":\"r5\"") != std::string::npos);
		// Malformed input has no readable requestId — the error goes without one.
		toWeb.clear();
		bridge.HandleWebMessage("someview", "not json at all");
		CHECK(toWeb.size() == 1);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"code\":\"malformed-message\"") != std::string::npos);
		CHECK(!toWeb.empty() && toWeb.back().second.find("requestId") == std::string::npos);

		// --- runtime.ready carries the host + protocol versions ---------------
		toWeb.clear();
		bridge.SendRuntimeReady("someview");
		CHECK(toWeb.size() == 1);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"version\":") != std::string::npos);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"bridgeVersion\":\"1.0\"") != std::string::npos);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"capabilities\"") == std::string::npos);
	}

	// --- RegisterView takes qualified ids only (item 1) -----------------------
	CHECK(!api.RegisterView("osf"));              // unqualified: refused synchronously
	CHECK(!api.RegisterView("osfui.settings"));   // dotted join, not slash
	CHECK(!api.RegisterView("Acme.Mod/dash"));    // bad mod id
	CHECK(api.RegisterView("acme.mymod/dash"));   // queued
	CHECK(api.RegisterView("osfui/settings"));    // dotless built-in mod id is legal
	{
		const auto regs = api.TakeViewRegistrations();
		CHECK(regs.size() == 2);
		CHECK(regs.size() == 2 && regs[0] == "acme.mymod/dash" && regs[1] == "osfui/settings");
	}

	// --- the Client wrapper (item 4): version-gated calls ---------------------
	{
		using OSFUI::API::Client;
		using OSFUI::API::Feature;

		Client c;
		CHECK(!c.IsConnected());
		CHECK(!c.Has(Feature::kCommands));           // unattached: everything gates off
		CHECK(!c.RequestMenu("osfui/settings", true));
		CHECK(c.GetSettingString("a.b", "k", nullptr, 0) == 0);
		CHECK(c.GetBridgeProtocolVersion() != nullptr);  // "" — never a null crash

		CHECK(c.Attach(&api));
		CHECK(c.IsConnected() && static_cast<bool>(c));
		CHECK(c.GetInterfaceVersion() == OSFUI::API::kBridgeAPIVersion);
		// Feature values are the introducing MINOR — a 1.6 host has them all.
		CHECK(c.Has(Feature::kCommands) && c.Has(Feature::kRequestMenu) &&
		      c.Has(Feature::kSettings) && c.Has(Feature::kDeliveryGuarantee) &&
		      c.Has(Feature::kHotkeys) && c.Has(Feature::kRegisterView) &&
		      c.Has(Feature::kCommandShape));
		CHECK(c.Raw() == &api);

		// Gated pass-throughs reach the real implementation.
		CHECK(c.RegisterView("acme.mymod/extra"));
		CHECK(!c.RegisterView("unqualified"));
		{
			const auto regs = api.TakeViewRegistrations();
			CHECK(regs.size() == 1 && regs[0] == "acme.mymod/extra");
		}
		c.Attach(nullptr);
		CHECK(!c.IsConnected());
	}

	// --- teardown: bridge going away must not dangle --------------------------
	api.OnBridgeReady(nullptr);
	api.PumpMainThread();
	CHECK(!api.IsBridgeReady());

	std::fprintf(stderr, "bridge_api_tests: %d checks, %d failure(s)\n", g_checks, g_failures);
	return g_failures;
}
