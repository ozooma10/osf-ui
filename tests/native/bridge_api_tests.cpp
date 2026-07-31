// Host-side unit tests for BridgeApi (api-freeze-plan items 1 + 3): the REAL
// src/api/BridgeApi.cpp compiled against stubs/pch.h — command-shape
// enforcement ("<author>.<modname>.<name>", ABI 1.6), first-wins duplicate
// refusal, unregister-then-reregister replacement, qualified RegisterView ids,
// and the registry-apply/dispatch round trip through a real MessageBridge.
// NOTE: BridgeApi is a process singleton — sections share state, in order.

#include "api/BridgeApi.h"
#include "OSFUI_JSON.h"

#include "core/Log.h"
#include "core/Version.h"  // kBridgeProtocolVersion (runtime.ready assertion)
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
	std::optional<OSFUI::API::Request> g_request;
	std::vector<OSFUI::API::Request> g_requests;
	std::uint64_t g_jsonToken = 0;
	std::string g_jsonType, g_jsonPayload, g_jsonCode, g_jsonMessage;
	void CaptureJsonResponse(std::uint64_t a_token, const char* a_type, const char* a_payload) noexcept
	{
		g_jsonToken = a_token;
		g_jsonType = a_type ? a_type : "";
		g_jsonPayload = a_payload ? a_payload : "";
	}
	void CaptureJsonRejection(std::uint64_t a_token, const char* a_code, const char* a_message) noexcept
	{
		g_jsonToken = a_token;
		g_jsonCode = a_code ? a_code : "";
		g_jsonMessage = a_message ? a_message : "";
	}
	std::string g_requestCommand, g_requestPayload, g_requestSource;
	void RequestHandler(const OSFUI::API::Request& a_request, void*) noexcept
	{
		g_request = a_request;
		g_requests.push_back(a_request);
		g_requestCommand = a_request.command; g_requestPayload = a_request.payloadJson; g_requestSource = a_request.sourceViewId;
	}
	struct OldHost final : OSFUI::API::IOSFUIBridge
	{
		int requestCalls{ 0 };
		std::uint32_t GetInterfaceVersion() override { return (1u << 16) | 6u; }
		void GetPluginVersion(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c) override { a=b=c=0; }
		const char* GetBridgeProtocolVersion() override { return "1.4"; }
		bool IsBridgeReady() override { return false; }
		void RegisterCommand(const char*, OSFUI::API::CommandFn, void*) override {}
		void UnregisterCommand(const char*) override {}
		bool SendToWeb(const char*, const char*, const char*) override { return false; }
		void SetReadyCallback(OSFUI::API::ReadyFn, void*) override {}
		bool RequestMenu(const char*, bool) override { return false; }
		std::uint32_t SubscribeSettings(const char*, OSFUI::API::SettingChangedFn, void*) override { return 0; }
		void UnsubscribeSettings(std::uint32_t) override {}
		bool GetSettingBool(const char*, const char*, bool*) override { return false; }
		bool GetSettingInt(const char*, const char*, std::int64_t*) override { return false; }
		bool GetSettingFloat(const char*, const char*, double*) override { return false; }
		std::uint32_t GetSettingString(const char*, const char*, char*, std::uint32_t) override { return 0; }
		bool RegisterSettingsSchema(const char*) override { return false; }
		void UnregisterSettingsSchema(const char*) override {}
		std::uint32_t SubscribeHotkey(const char*, const char*, OSFUI::API::HotkeyFn, void*) override { return 0; }
		void UnsubscribeHotkey(std::uint32_t) override {}
		bool RegisterView(const char*) override { return false; }
		bool ReportIssue(const char*, const char*, const char*, std::uint32_t, const char*, const char*) override { return false; }
		bool ClearIssue(const char*, const char*) override { return false; }
		bool ClearIssuesExcept(const char*, const char*) override { return false; }
		void RegisterRequest(const char*, OSFUI::API::RequestFn, void*) override { ++requestCalls; }
		void UnregisterRequest(const char*) override { ++requestCalls; }
	};
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
	api.SetViewCatalog({ "someview" });
	api.SetSurfaceLoaded("someview", true);

	// --- version constants: 1.7 (diagnostics and request/response) ------------
	CHECK(OSFUI::API::kBridgeAPIMajor == 1);
	CHECK(OSFUI::API::kBridgeAPIMinor == 7);
	CHECK(static_cast<std::uint32_t>(OSFUI::API::Feature::kDiagnostics) == 7);
	CHECK(static_cast<std::uint32_t>(OSFUI::API::Feature::kRequests) == 7);
	CHECK(api.GetInterfaceVersion() == OSFUI::API::kBridgeAPIVersion);
	CHECK(std::string_view(OSFUI::kBridgeProtocolVersion) == "1.5");

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
		// Built from the constant, not a literal — the version bumps on additive
		// protocol changes and this check is about the FIELD being present.
		CHECK(!toWeb.empty() && toWeb.back().second.find(std::format("\"bridgeVersion\":\"{}\"", OSFUI::kBridgeProtocolVersion)) != std::string::npos);
		CHECK(!toWeb.empty() && toWeb.back().second.find("\"capabilities\"") == std::string::npos);
	}

	// --- ABI 1.7 request/response ---------------------------------------------
	api.RegisterRequest("acme.mymod.getWeight", &RequestHandler, nullptr);
	api.RegisterRequest("acme.mymod.getWeight", &RequestHandler, nullptr); // duplicate refused
	api.RegisterRequest("acme.mymod.ping", &RequestHandler, nullptr);      // command/request collision refused
	api.RegisterCommand("acme.mymod.getWeight", &HandlerA, nullptr);       // reverse collision refused
	api.PumpMainThread();
	CHECK(LoggedContaining("WARN", "refused RegisterRequest('acme.mymod.getWeight')"));

	// Deferred success carries the plugin's type and original requestId, never an ack.
	toWeb.clear(); g_request.reset();
	bridge.HandleWebMessage("request-view", R"({"type":"ui.command","requestId":"rq1","payload":{"command":"acme.mymod.getWeight","unit":"kg"}})");
	CHECK(g_request.has_value()); CHECK(toWeb.empty());
	if (g_request) {
		CHECK(g_requestCommand == "acme.mymod.getWeight");
		CHECK(g_requestPayload.find("requestId") == std::string::npos);
		g_request->Respond("acme.mymod.weight", R"({"weight":42.5})");
		g_request->Respond("acme.mymod.weight", R"({"weight":99})"); // ignored
	}
	api.PumpMainThread();
	CHECK(toWeb.size() == 1);
	CHECK(!toWeb.empty() && toWeb.back().second.find("\"type\":\"acme.mymod.weight\"") != std::string::npos);
	CHECK(!toWeb.empty() && toWeb.back().second.find("\"requestId\":\"rq1\"") != std::string::npos);
	CHECK(LoggedContaining("WARN", "ignored second response"));

	// A dropped token times out as correlated ui.error { no-response }.
	toWeb.clear(); g_request.reset();
	bridge.HandleWebMessage("request-view", R"({"type":"ui.command","requestId":"rq2","payload":{"command":"acme.mymod.getWeight"}})");
	api.PumpMainThread(std::chrono::steady_clock::now() + std::chrono::seconds(31));
	CHECK(toWeb.size() == 1);
	CHECK(!toWeb.empty() && toWeb.back().second.find("\"code\":\"no-response\"") != std::string::npos);

	// Closing the source view reaps its token; a late response is a safe no-op.
	toWeb.clear(); g_request.reset();
	bridge.HandleWebMessage("closing-view", R"({"type":"ui.command","requestId":"rq3","payload":{"command":"acme.mymod.getWeight"}})");
	api.SetSurfaceLoaded("closing-view", false);
	if (g_request) g_request->Respond(R"({"weight":1})");
	api.PumpMainThread();
	CHECK(toWeb.empty());
	CHECK(LoggedContaining("WARN", "ignored late response"));

	// Plugin failures become correlated ui.error with the plugin's stable code.
	toWeb.clear(); g_request.reset();
	bridge.HandleWebMessage("request-view", R"({"type":"ui.command","requestId":"rq4","payload":{"command":"acme.mymod.getWeight"}})");
	if (g_request) g_request->Reject("weight-unavailable", "no player");
	api.PumpMainThread();
	CHECK(toWeb.size() == 1);
	CHECK(!toWeb.empty() && toWeb.back().second.find("\"code\":\"weight-unavailable\"") != std::string::npos);

	// The 65th in-flight request from one view fails fast instead of growing.
	toWeb.clear(); g_requests.clear();
	for (int i = 0; i < 65; ++i) {
		bridge.HandleWebMessage("cap-view", std::format(
			R"({{"type":"ui.command","requestId":"cap{}","payload":{{"command":"acme.mymod.getWeight"}}}})", i));
	}
	CHECK(g_requests.size() == 64);
	CHECK(toWeb.size() == 1);
	CHECK(!toWeb.empty() && toWeb.back().second.find("\"code\":\"request-capacity\"") != std::string::npos);
	api.SetSurfaceLoaded("cap-view", false);	// --- RegisterView takes qualified ids only (item 1) -----------------------
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

	// --- RequestMenu validates against discovery at queue time ----------------
	api.SetViewCatalog({ "acme.mymod/dash", "osfui/settings" });
	CHECK(!api.RequestMenu("acme.mymod/missing", true));  // typo: synchronous fallback signal

	// Model a boot with no nativeBridge surface: sends stay queued while the API
	// is not ready. Publishing the bridge as the discovered surface is loaded on
	// demand flushes them, and web messages from that first surface are handled.
	api.OnBridgeReady(nullptr);
	api.PumpMainThread();
	CHECK(!api.IsBridgeReady());
	CHECK(api.SendToWeb("acme.mymod/dash", "acme.mymod.data", R"({"lazy":true})"));
	CHECK(api.RequestMenu("acme.mymod/dash", true));      // discovered, not loaded: accepted
	CHECK(!api.RequestMenu("acme.mymod/dash", false));    // close must not lazy-load
	{
		const auto requests = api.TakeMenuRequests();
		CHECK(requests.size() == 1);
		if (requests.size() == 1) {
			CHECK(requests[0].view == "acme.mymod/dash");
			CHECK(requests[0].open);
		}
	}
	toWeb.clear();
	MessageBridge lazyBridge([&](std::string_view a_viewId, std::string_view a_json) {
		toWeb.emplace_back(std::string(a_viewId), std::string(a_json));
	});
	api.OnBridgeReady(&lazyBridge);
	api.PumpMainThread();
	CHECK(api.IsBridgeReady());
	CHECK(toWeb.empty());  // a warm bridge alone does not make this target live
	api.SetSurfaceLoaded("acme.mymod/dash", true);
	lazyBridge.SendRuntimeReady("acme.mymod/dash");
	api.PumpMainThread();
	CHECK(toWeb.size() == 2);
	CHECK(toWeb.size() == 2 && toWeb[0].second.find("\"type\":\"runtime.ready\"") != std::string::npos);
	CHECK(toWeb.size() == 2 && toWeb[1].first == "acme.mymod/dash");
	CHECK(toWeb.size() == 2 && toWeb[1].second.find("\"lazy\":true") != std::string::npos);
	lazyBridge.HandleWebMessage("acme.mymod/dash",
		R"({ "type": "ui.command", "payload": { "command": "acme.mymod.catalog.get" } })");
	CHECK(g_firedA.size() == 3);  // handler is wired on the first lazy bridge
	api.OnBridgeReady(nullptr);
	api.PumpMainThread();

	CHECK(api.RequestMenu("acme.mymod/dash", false));
	{
		const auto requests = api.TakeMenuRequests();
		CHECK(requests.size() == 1);
		if (requests.size() == 1) {
			CHECK(requests[0].view == "acme.mymod/dash");
			CHECK(!requests[0].open);
		}
	}

	// Holdback stays bounded even while another bridge is live: the 65th send
	// drops the oldest, so a state-like stream converges on the newest 64.
	api.SetSurfaceLoaded("acme.mymod/dash", false);
	api.OnBridgeReady(&lazyBridge);
	api.PumpMainThread();
	toWeb.clear();
	for (int i = 0; i < 65; ++i) {
		CHECK(api.SendToWeb("acme.mymod/dash", "acme.mymod.state",
			std::format(R"({{"seq":{}}})", i).c_str()));
	}
	api.PumpMainThread();
	CHECK(toWeb.empty());
	CHECK(LoggedContaining("WARN", "SendToWeb holdback"));
	api.SetSurfaceLoaded("acme.mymod/dash", true);
	api.PumpMainThread();
	CHECK(toWeb.size() == 64);
	if (toWeb.size() == 64) {
		const auto first = nlohmann::json::parse(toWeb.front().second, nullptr, false);
		const auto last = nlohmann::json::parse(toWeb.back().second, nullptr, false);
		CHECK(first["payload"]["seq"] == 1);
		CHECK(last["payload"]["seq"] == 64);
	}
	toWeb.clear();
	CHECK(api.SendToWeb("acme.mymod/missing", "acme.mymod.state", R"({"x":1})"));
	api.PumpMainThread();
	CHECK(toWeb.empty());
	api.OnBridgeReady(nullptr);
	api.PumpMainThread();

	// --- the Client wrapper (item 4): version-gated calls ---------------------
	{
		using OSFUI::API::Client;
		using OSFUI::API::Feature;

		Client c;
		OldHost oldHost;
		CHECK(c.Attach(&oldHost));
		CHECK(!c.Has(Feature::kDiagnostics));
		CHECK(!c.Has(Feature::kRequests));
		c.RegisterRequest("acme.mymod.old", &RequestHandler, nullptr);
		c.UnregisterRequest("acme.mymod.old");
		CHECK(oldHost.requestCalls == 0);
		c.Attach(nullptr);
		CHECK(!c.IsConnected());
		CHECK(!c.Has(Feature::kCommands));           // unattached: everything gates off
		CHECK(!c.RequestMenu("osfui/settings", true));
		CHECK(c.GetSettingString("a.b", "k", nullptr, 0) == 0);
		CHECK(c.GetBridgeProtocolVersion() != nullptr);  // "" — never a null crash

		CHECK(c.Attach(&api));
		CHECK(c.IsConnected() && static_cast<bool>(c));
		CHECK(c.GetInterfaceVersion() == OSFUI::API::kBridgeAPIVersion);
		// Feature values are the introducing MINOR — the current 1.7 host has them all.
		CHECK(c.Has(Feature::kCommands) && c.Has(Feature::kRequestMenu) &&
		      c.Has(Feature::kSettings) && c.Has(Feature::kDeliveryGuarantee) &&
		      c.Has(Feature::kHotkeys) && c.Has(Feature::kRegisterView) &&
		      c.Has(Feature::kCommandShape) && c.Has(Feature::kDiagnostics) &&
		      c.Has(Feature::kRequests));
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

	// --- health reporting (ABI 1.7) -------------------------------------------
	// Validation is synchronous; the ops queue for the main tick, where Runtime
	// namespaces them into the registry.
	{
		using Op = OSFUI::API::BridgeApi::DiagnosticOp;
		using Sev = OSFUI::API::IssueSeverity;
		const auto kWarn = static_cast<std::uint32_t>(Sev::kWarning);
		const auto kErr = static_cast<std::uint32_t>(Sev::kError);

		api.TakeDiagnosticOps();  // start from an empty queue

		// The caller's mod id is the source, so it must be a real mod id.
		CHECK(!api.ReportIssue(nullptr, "x", "y", kWarn, "", nullptr));
		CHECK(!api.ReportIssue("", "x", "y", kWarn, "", nullptr));
		CHECK(!api.ReportIssue("Acme.Mod", "x", "y", kWarn, "", nullptr));
		CHECK(!api.ReportIssue("settings", "x", "y", kWarn, "", nullptr));  // a platform source
		CHECK(LoggedContaining("WARN", "refused ReportIssue('Acme.Mod')"));
		// Identity and kind are both required.
		CHECK(!api.ReportIssue("acme.mymod", "", "y", kWarn, "", nullptr));
		CHECK(!api.ReportIssue("acme.mymod", "x", "", kWarn, "", nullptr));
		// Context must be an object when present — an array or a scalar is a
		// producer bug, reported at the call rather than silently dropped.
		CHECK(!api.ReportIssue("acme.mymod", "x", "y", kWarn, "", "[1,2]"));
		CHECK(!api.ReportIssue("acme.mymod", "x", "y", kWarn, "", "\"nope\""));
		CHECK(!api.ReportIssue("acme.mymod", "x", "y", kWarn, "", "{not json"));
		CHECK(api.TakeDiagnosticOps().empty());  // nothing invalid was queued

		// Accepted: null/empty context means "none"; an unknown severity is
		// treated as the worst tier known rather than silently softened.
		CHECK(api.ReportIssue("acme.mymod", "pack-parse", "catalog.parse-failed", kErr,
			"highlights", "{\"file\":\"C:\\\\Mods\\\\packs\\\\bad.json\",\"line\":12}"));
		CHECK(api.ReportIssue("acme.mymod", "quiet", "audio.missing", kWarn, "", nullptr));
		CHECK(api.ReportIssue("acme.mymod", "future", "odd.tier", 9u, "", ""));
		CHECK(LoggedContaining("WARN", "unknown severity 9"));
		CHECK(api.ClearIssue("acme.mymod", "quiet"));
		CHECK(!api.ClearIssue("acme.mymod", ""));
		CHECK(!api.ClearIssue("bogus id", "quiet"));
		CHECK(api.ClearIssuesExcept("acme.mymod", "[\"pack-parse\"]"));
		CHECK(api.ClearIssuesExcept("acme.mymod", "[]"));   // sweep all of mine
		CHECK(api.ClearIssuesExcept("acme.mymod", nullptr));  // same, absent payload
		CHECK(!api.ClearIssuesExcept("acme.mymod", "{\"a\":1}"));
		CHECK(!api.ClearIssuesExcept("acme.mymod", "[\"ok\", 7]"));

		const auto ops = api.TakeDiagnosticOps();
		// FIFO across kinds: a report-then-sweep pair must land in call order.
		CHECK(ops.size() == 7);
		CHECK(ops[0].kind == Op::Kind::kReport && ops[0].modId == "acme.mymod");
		CHECK(ops[0].id == "pack-parse" && ops[0].code == "catalog.parse-failed");
		CHECK(ops[0].error && ops[0].subject == "highlights");
		// The context rides through verbatim — redaction is the registry's job,
		// so the path here is still whole at this layer.
		CHECK(ops[0].context.value("line", 0) == 12);
		CHECK(!ops[1].error);
		CHECK(ops[2].error);  // severity 9 -> error
		CHECK(ops[3].kind == Op::Kind::kClear && ops[3].id == "quiet");
		CHECK(ops[4].kind == Op::Kind::kClearExcept && ops[4].keep.size() == 1 &&
		      ops[4].keep[0] == "pack-parse");
		CHECK(ops[5].kind == Op::Kind::kClearExcept && ops[5].keep.empty());
		CHECK(ops[6].kind == Op::Kind::kClearExcept && ops[6].keep.empty());
		CHECK(api.TakeDiagnosticOps().empty());  // drained

		// A producer looping off-thread hits the queue cap instead of growing
		// memory; the refusal is the incoming op, so earlier state is kept.
		for (int i = 0; i < 300; ++i) {
			api.ReportIssue("acme.mymod", ("id" + std::to_string(i)).c_str(), "spam", kWarn, "", nullptr);
		}
		const auto capped = api.TakeDiagnosticOps();
		CHECK(capped.size() == 256);
		CHECK(capped.front().id == "id0");  // oldest kept
		CHECK(LoggedContaining("WARN", "health reports already queued"));
	}

	// --- optional JSON authoring facade ---------------------------------------
	{
		using OSFUI::API::Json;
		using OSFUI::API::JsonCommand;
		using OSFUI::API::JsonRequest;

		JsonCommand command{ "acme.mymod.set", R"({"weight":42.5,"name":"ore"})", "acme.mymod/view" };
		CHECK(command.IsValid());
		CHECK(command.Command() == "acme.mymod.set");
		CHECK(command.SourceViewId() == "acme.mymod/view");
		CHECK(command.Require<double>("weight") == 42.5);
		CHECK(command.Value<std::string>("name", "") == "ore");
		CHECK(command.Value<int>("missing", 7) == 7);
		int numeric = 0;
		CHECK(!command.TryGet("name", numeric));  // wrong field type returns false

		JsonCommand malformed{ "x", "{bad", "v" };
		CHECK(!malformed.IsValid());
		CHECK(!malformed.Error().empty());

		OSFUI::API::Request raw;
		raw.command = "acme.mymod.getWeight";
		raw.payloadJson = R"({"formId":42})";
		raw.sourceViewId = "acme.mymod/view";
		raw._token = 91;
		raw._respond = &CaptureJsonResponse;
		raw._reject = &CaptureJsonRejection;
		JsonRequest request{ raw };
		CHECK(request.IsValid());
		const auto formId = request.Get<int>("formId");
		CHECK(formId && *formId == 42);
		CHECK(request.Respond("acme.mymod.weight", Json{ { "weight", 12.5 } }));
		CHECK(g_jsonToken == 91 && g_jsonType == "acme.mymod.weight");
		CHECK(Json::parse(g_jsonPayload).at("weight") == 12.5);

		g_jsonCode.clear();
		raw.payloadJson = "[1,2]";
		JsonRequest badRequest{ raw };
		CHECK(!badRequest.IsValid());
		CHECK(g_jsonCode == "invalid-payload");

		g_jsonCode.clear();
		raw.payloadJson = R"({"formId":"wrong"})";
		JsonRequest wrongField{ raw };
		CHECK(!wrongField.Get<int>("formId"));
		CHECK(g_jsonCode == "invalid-payload");
	}
	// --- teardown: bridge going away must not dangle --------------------------
	api.OnBridgeReady(nullptr);
	api.PumpMainThread();
	CHECK(!api.IsBridgeReady());

	std::fprintf(stderr, "bridge_api_tests: %d checks, %d failure(s)\n", g_checks, g_failures);
	return g_failures;
}
