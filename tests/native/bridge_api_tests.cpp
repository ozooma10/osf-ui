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
