// Host-side unit tests for BridgeApi ABI 2.0 + MessageBridge protocol 2.0
// (docs/mod-api-2.0-design.md): the REAL src/api/BridgeApi.cpp and
// src/runtime/MessageBridge.cpp compiled against stubs/pch.h. Pins the endpoint
// grammar ("<author>.<modname>.<name>", api-freeze item 3), first-wins duplicate
// refusal, unregister-then-reregister replacement, qualified RegisterView ids
// (item 1), and — end to end through a live bridge — the four-verb envelope, the
// page-initiated handshake, strict kind enforcement, request settlement,
// retained state and the refused-ABI caller ledger.
//
// The 1.x wire is gone, so every inbound message here is a 2.0 envelope with the
// routing metadata BESIDE the payload:
//     { "kind":"send",    "name":..., "payload":{...} }
//     { "kind":"request", "name":..., "id":..., "payload":{...} }
// and every outbound one is ready / state / event / reply / error.
//
// NOTE: BridgeApi is a process singleton — the sections below share state and
// MUST run in declaration order; each assumes what the one above it left behind.

#include "api/BridgeApi.h"
#include "OSFUI_JSON.h"

#include "core/Log.h"
#include "core/Version.h"  // kPluginVersion / kBridgeProtocolVersion (the `ready` payload)
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

	// --- the 2.0 wire, written once ---------------------------------------
	// Every inbound message in this suite goes through these two builders, so
	// the envelope shape is asserted by construction rather than re-typed (and
	// re-mistyped) at forty call sites.
	constexpr const char* kHello = R"({"kind":"send","name":"osfui.hello"})";

	std::string SendMsg(std::string_view a_name, std::string_view a_payloadJson = "{}")
	{
		return std::format(R"({{"kind":"send","name":"{}","payload":{}}})", a_name, a_payloadJson);
	}
	std::string RequestMsg(std::string_view a_name, std::string_view a_id,
		std::string_view a_payloadJson = "{}")
	{
		return std::format(R"({{"kind":"request","name":"{}","id":"{}","payload":{}}})",
			a_name, a_id, a_payloadJson);
	}

	// (viewId, raw json) pairs captured from the bridge's transport.
	using Outbox = std::vector<std::pair<std::string, std::string>>;

	// Accessors that never throw: a missing or malformed envelope yields an
	// empty object, so a wrong shape fails its CHECK instead of taking the
	// process down and hiding every check after it.
	nlohmann::json Envelope(const Outbox& a_out, std::size_t a_index)
	{
		if (a_index >= a_out.size()) {
			return nlohmann::json::object();
		}
		auto parsed = nlohmann::json::parse(a_out[a_index].second, nullptr, false);
		return parsed.is_object() ? parsed : nlohmann::json::object();
	}
	nlohmann::json Last(const Outbox& a_out)
	{
		return a_out.empty() ? nlohmann::json::object() : Envelope(a_out, a_out.size() - 1);
	}
	nlohmann::json PayloadOf(const nlohmann::json& a_envelope)
	{
		return a_envelope.value("payload", nlohmann::json::object());
	}

	// Recorded send-handler firings.
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

	// Host-detected protocol misuse. In the plugin this reaches the offending
	// view as a dev-only `osfui.debug.error` event and feeds the release-mode
	// `view.protocol-misuse` card; here it is the assertion channel for every
	// refusal that has no reply envelope to carry it (a send cannot be
	// answered, and malformed input has no id to correlate to).
	struct Surfaced
	{
		std::string    view;
		std::string    code;
		std::string    message;
		nlohmann::json detail;
	};
	std::vector<Surfaced> g_surfaced;
	std::string LastSurfacedCode()
	{
		return g_surfaced.empty() ? std::string{} : g_surfaced.back().code;
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

	// Complete ABI 2.0 host double for Client pass-through checks.
	struct TestHost final : OSFUI::API::IOSFUIBridge
	{
		int requestCalls{ 0 };
		std::uint32_t GetInterfaceVersion() override { return OSFUI::API::kBridgeAPIVersion; }
		void GetPluginVersion(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c) override { a=b=c=0; }
		const char* GetBridgeProtocolVersion() override { return "1.4"; }
		bool IsBridgeReady() override { return false; }
		void RegisterSend(const char*, OSFUI::API::SendFn, void*) override {}
		void UnregisterSend(const char*) override {}
		bool SendToWeb(const char*, const char*, const char*) override { return false; }
		bool SetViewState(const char*, const char*, const char*) override { return false; }
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

	// --- version constants: breaking ABI 2.0 ----------------------------------
	CHECK(OSFUI::API::kBridgeAPIMajor == 2);
	CHECK(OSFUI::API::kBridgeAPIMinor == 0);
	CHECK(OSFUI::API::kBridgeAPIVersion == ((2u << 16) | 0u));
	CHECK(static_cast<std::uint32_t>(OSFUI::API::Feature::kViewState) == 0);
	CHECK(api.GetInterfaceVersion() == OSFUI::API::kBridgeAPIVersion);
	CHECK(std::string_view(OSFUI::kBridgeProtocolVersion) == "2.0");
	CHECK(std::string_view(api.GetBridgeProtocolVersion()) == "2.0");

	// --- endpoint shape (item 3): two dots minimum, item-1 mod-id grammar -----
	// Every platform endpoint is structurally unregisterable — dotless verbs,
	// single-dot names (including the osfui.* built-ins), bad mod ids. The 2.0
	// rename changed which names the platform owns, not the rule that owns them.
	for (const auto* bad : { "close", "ping", "setVisible", "menu.open", "game.get",
	                         "settings.set", "papyrus.call", "papyrus.send", "osfui.hello",
	                         "osfui.gamepadRaw",
	                         "Acme.Mod.x", "under_score.mod.x", "acme.mymod.",
	                         ".leading.x", "a..b.x" }) {
		api.RegisterSend(bad, &HandlerA, nullptr);
	}
	CHECK(LoggedContaining("WARN", "refused RegisterSend('close')"));
	CHECK(LoggedContaining("WARN", "refused RegisterSend('menu.open')"));
	CHECK(LoggedContaining("WARN", "refused RegisterSend('osfui.hello')"));
	CHECK(LoggedContaining("WARN", "refused RegisterSend('papyrus.send')"));
	CHECK(LoggedContaining("WARN", "refused RegisterSend('Acme.Mod.x')"));

	// Accepted: "<author>.<modname>.<name>", name may itself contain dots.
	api.RegisterSend("acme.mymod.ping", &HandlerA, nullptr);
	api.RegisterSend("acme.mymod.catalog.get", &HandlerA, nullptr);

	// --- duplicates: first-wins, refused (item 3) -----------------------------
	api.RegisterSend("acme.mymod.ping", &HandlerB, nullptr);  // hijack attempt
	CHECK(LoggedContaining("WARN", "refused RegisterSend('acme.mymod.ping') — already registered"));

	// --- apply to a live bridge -----------------------------------------------
	Outbox toWeb;
	MessageBridge bridge([&](std::string_view a_viewId, std::string_view a_json) {
		toWeb.emplace_back(std::string(a_viewId), std::string(a_json));
	});
	bridge.SetSurfaceFn([](std::string_view a_view, std::string_view a_code,
						   std::string_view a_message, const nlohmann::json& a_detail, bool) {
		g_surfaced.push_back({ std::string(a_view), std::string(a_code), std::string(a_message), a_detail });
	});
	api.OnBridgeReady(&bridge);
	api.PumpMainThread();

	// --- the handshake is PAGE-INITIATED and is the only boot path ------------
	// A fresh document says hello; the bridge answers `ready`, replays state
	// through Runtime's hook (not installed here), then opens the event gate.
	// First open, F5, dev hot reload and crash-recovery reload are all this same
	// sequence, so nothing has to guess whether a greeting was consumed.
	bridge.OnViewCreated("someview");
	CHECK(!bridge.HasGreeted("someview"));
	toWeb.clear();
	bridge.HandleWebMessage("someview", kHello);
	CHECK(bridge.HasGreeted("someview"));
	CHECK(toWeb.size() == 1);
	{
		const auto ready = Last(toWeb);
		const auto info = PayloadOf(ready);
		CHECK(ready.value("kind", "") == "ready");
		CHECK(info.value("game", "") == "Starfield");
		CHECK(info.value("plugin", "") == OSFUI::kPluginName);
		CHECK(info.value("version", "") == OSFUI::kPluginVersion);
		// Built from the constant, not a literal — the version bumps on additive
		// protocol changes and this check is about the FIELD being present.
		CHECK(info.value("bridgeVersion", "") == OSFUI::kBridgeProtocolVersion);
		// `view`/`mod` are what let a document address its own state keys
		// without hardcoding its id. An unqualified id is its own mod.
		CHECK(info.value("view", "") == "someview");
		CHECK(info.value("mod", "") == "someview");
		CHECK(!info.contains("capabilities"));  // removed pre-1.0, still gone
	}

	// --- ABI sends are EVENTS -------------------------------------------------
	// SendToWeb validates once, retains the payload text while queued, and comes
	// out of the main-thread pump as the 2.0 event envelope. `type` no longer
	// exists: the name is routing metadata beside the payload.
	toWeb.clear();
	CHECK(api.SendToWeb("someview", "acme.mymod.data", R"({"x":1,"label":"ok"})"));
	CHECK(!api.SendToWeb("someview", "acme.mymod.data", "{ bad json"));
	api.PumpMainThread();
	CHECK(toWeb.size() == 1);
	{
		const auto message = Last(toWeb);
		CHECK(message.value("kind", "") == "event");
		CHECK(message.value("name", "") == "acme.mymod.data");
		CHECK(PayloadOf(message).value("x", 0) == 1);
		CHECK(PayloadOf(message).value("label", "") == "ok");
		CHECK(!message.contains("type"));
	}

	// Multi-target emits encode one identical envelope and retain target ids.
	// Both targets must have greeted: an event is a one-shot happening, so a
	// document that has not asked for anything yet is not a delivery target.
	for (const auto* id : { "view-a", "view-b" }) {
		bridge.OnViewCreated(id);
		bridge.HandleWebMessage(id, kHello);
	}
	toWeb.clear();
	bridge.Emit(std::unordered_set<std::string>{ "view-a", "view-b" },
		"test.broadcast", nlohmann::json{ { "large", nlohmann::json::array({ 1, 2, 3 }) } });
	CHECK(toWeb.size() == 2);
	if (toWeb.size() == 2) {
		CHECK(toWeb[0].second == toWeb[1].second);
		CHECK(toWeb[0].first != toWeb[1].first);
		const auto message = Envelope(toWeb, 0);
		CHECK(message.value("kind", "") == "event");
		CHECK(message.value("name", "") == "test.broadcast");
		CHECK(PayloadOf(message).value("large", nlohmann::json{}) == nlohmann::json::array({ 1, 2, 3 }));
	}

	// --- RegisterSend is strictly one-way --------------------------------------
	// send() carries the payload verbatim.
	toWeb.clear();
	bridge.HandleWebMessage("someview", SendMsg("acme.mymod.ping", R"({"x":1})"));
	CHECK(g_firedA.size() == 1);
	CHECK(g_firedB.empty());  // the duplicate registration never took
	if (!g_firedA.empty()) {
		CHECK(g_firedA[0].command == "acme.mymod.ping");
		CHECK(g_firedA[0].source == "someview");
		CHECK(g_firedA[0].payload == R"({"x":1})");
		CHECK(g_firedA[0].payload.find("requestId") == std::string::npos);
	}
	CHECK(toWeb.empty());  // no auto-ack

	// An absent payload is an empty object, never null.
	bridge.HandleWebMessage("someview", SendMsg("acme.mymod.catalog.get"));
	CHECK(g_firedA.size() == 2);
	CHECK(g_firedA.back().payload == "{}");
	CHECK(toWeb.empty());

	// A refused (platform-shaped) registration must not exist on the bridge: the
	// platform verb resolves to nothing here (no core handler in this harness),
	// and crucially NOT to HandlerA. The drop is SURFACED, which is the part 1.x
	// got wrong — a silently swallowed message looks like a hang to the page.
	g_surfaced.clear();
	bridge.HandleWebMessage("someview", SendMsg("close"));
	CHECK(g_firedA.size() == 2);
	CHECK(toWeb.empty());
	CHECK(LastSurfacedCode() == "unknown-endpoint");
	CHECK(LoggedContaining("WARN", "dropped send to unknown endpoint 'close'"));

	// --- envelope hygiene: routing metadata is structural, not advisory -------
	{
		// `id` on a send: the caller expects a settlement it will never get.
		// Answering it would resurrect the auto-ack.
		g_surfaced.clear();
		bridge.HandleWebMessage("someview",
			R"({"kind":"send","name":"acme.mymod.ping","id":"x","payload":{}})");
		CHECK(g_firedA.size() == 2);
		CHECK(toWeb.empty());
		CHECK(LastSurfacedCode() == "invalid-request");

		// A present-but-non-object payload is a client bug, not something to
		// coerce — coercing is how a payload field ends up deciding routing.
		bridge.HandleWebMessage("someview",
			R"({"kind":"send","name":"acme.mymod.ping","payload":[1,2]})");
		CHECK(g_firedA.size() == 2);
		CHECK(LastSurfacedCode() == "invalid-request");

		// The 1.x envelope itself routes nowhere: `kind` is closed to
		// send/request, and `command` inside the payload carries no authority.
		bridge.HandleWebMessage("someview",
			R"({"type":"ui.command","payload":{"command":"acme.mymod.ping"}})");
		CHECK(g_firedA.size() == 2);
		CHECK(LastSurfacedCode() == "invalid-request");

		bridge.HandleWebMessage("someview", R"({"kind":"send","name":""})");
		CHECK(LastSurfacedCode() == "invalid-request");

		// Malformed text has no id to correlate an error to, so the only honest
		// channels are the log and the offending view's own console — never an
		// uncorrelated error envelope that no caller is waiting on.
		toWeb.clear();
		bridge.HandleWebMessage("someview", "not json at all");
		CHECK(toWeb.empty());
		CHECK(LastSurfacedCode() == "invalid-request");
	}

	// A request naming a send is refused. It never invokes the callback, injects
	// routing fields, or fabricates an acknowledgement.
	toWeb.clear();
	bridge.HandleWebMessage("someview", RequestMsg("acme.mymod.ping", "q1"));
	CHECK(g_firedA.size() == 2);
	CHECK(toWeb.size() == 1);
	{
		const auto error = Last(toWeb);
		CHECK(error.value("kind", "") == "error");
		CHECK(error.value("id", "") == "q1");
		CHECK(PayloadOf(error).value("code", "") == "wrong-endpoint-kind");
	}

	// --- replacement is explicit: unregister, then re-register ----------------
	api.UnregisterSend("acme.mymod.ping");
	api.RegisterSend("acme.mymod.ping", &HandlerB, nullptr);  // now legal (slot free)
	api.PumpMainThread();
	toWeb.clear();
	bridge.HandleWebMessage("someview", SendMsg("acme.mymod.ping"));
	CHECK(g_firedA.size() == 2);
	CHECK(g_firedB.size() == 1);

	// --- request settlement ---------------------------------------------------
	// A request handler MUST settle exactly once — Respond, Reject, or Defer and
	// settle later by id. There is no third outcome and no auto-ack.
	{
		bridge.RegisterRequest("test.reply", [](const nlohmann::json&, MessageBridge& a_b) {
			a_b.Respond(nlohmann::json{ { "v", 7 } });
		});
		toWeb.clear();
		bridge.HandleWebMessage("someview", RequestMsg("test.reply", "r2", R"({"in":1})"));
		CHECK(toWeb.size() == 1);
		CHECK(Last(toWeb).value("kind", "") == "reply");
		CHECK(Last(toWeb).value("id", "") == "r2");
		CHECK(PayloadOf(Last(toWeb)).value("v", 0) == 7);

		// Reject: a stable machine code plus a human sentence, correlated by id.
		bridge.RegisterRequest("test.fail", [](const nlohmann::json&, MessageBridge& a_b) {
			a_b.Reject("unknown-view", "nope");
		});
		toWeb.clear();
		bridge.HandleWebMessage("someview", RequestMsg("test.fail", "r3"));
		CHECK(toWeb.size() == 1);
		CHECK(Last(toWeb).value("kind", "") == "error");
		CHECK(Last(toWeb).value("id", "") == "r3");
		CHECK(PayloadOf(Last(toWeb)).value("code", "") == "unknown-view");
		CHECK(PayloadOf(Last(toWeb)).value("message", "") == "nope");

		// Settling twice answers once. (1.x tracked this with a `_replied` flag
		// the call sites had to respect; here the bridge owns it.)
		bridge.RegisterRequest("test.twice", [](const nlohmann::json&, MessageBridge& a_b) {
			a_b.Respond(nlohmann::json::object());
			a_b.Respond(nlohmann::json{ { "second", true } });
		});
		toWeb.clear();
		bridge.HandleWebMessage("someview", RequestMsg("test.twice", "r3b"));
		CHECK(toWeb.size() == 1);
		CHECK(LoggedContaining("WARN", "settled twice"));

		// Defer + RespondTo: the settings.captureKey pattern. Nothing goes out
		// at handler time; the token Defer() returns is the handler's to settle
		// later, and the page's own id comes back on the wire.
		std::string deferToken;
		std::string deferredId;
		bridge.RegisterRequest("test.defer", [&](const nlohmann::json&, MessageBridge& a_b) {
			deferredId = std::string(a_b.CurrentRequestId());
			deferToken = a_b.Defer();
		});
		toWeb.clear();
		bridge.HandleWebMessage("someview", RequestMsg("test.defer", "r4"));
		CHECK(toWeb.empty());
		CHECK(deferredId == "r4");
		// The token is the HOST's, never the page's id — that is what keeps two
		// documents' "q1" apart.
		CHECK(!deferToken.empty() && deferToken != "r4");
		bridge.RespondTo(deferToken, nlohmann::json{ { "done", true } });
		CHECK(toWeb.size() == 1);
		CHECK(Last(toWeb).value("kind", "") == "reply");
		CHECK(Last(toWeb).value("id", "") == "r4");
		// A late or duplicate settlement is never delivered twice, and settling
		// a token nobody deferred is a silent no-op rather than a stray
		// envelope. The page's raw id is not a settlement handle either.
		bridge.RespondTo(deferToken, nlohmann::json{ { "done", false } });
		bridge.RejectTo("no-such-id", "internal", "");
		bridge.RespondTo("r4", nlohmann::json{ { "done", false } });
		CHECK(toWeb.size() == 1);

		// RejectTo settles a deferred request the same way.
		toWeb.clear();
		bridge.HandleWebMessage("someview", RequestMsg("test.defer", "r5"));
		bridge.RejectTo(deferToken, "capture-busy", "already capturing");
		CHECK(toWeb.size() == 1);
		CHECK(PayloadOf(Last(toWeb)).value("code", "") == "capture-busy");
		CHECK(Last(toWeb).value("id", "") == "r5");

		// Two documents both number their first request "q1". Keying deferrals
		// by that id alone routed one page's reply into the other's promise.
		toWeb.clear();
		std::vector<std::string> tokens;
		bridge.RegisterRequest("test.collide", [&tokens](const nlohmann::json&, MessageBridge& a_b) {
			tokens.push_back(a_b.Defer());
		});
		bridge.HandleWebMessage("view.a/panel", RequestMsg("test.collide", "q1"));
		bridge.HandleWebMessage("view.b/panel", RequestMsg("test.collide", "q1"));
		CHECK(tokens.size() == 2 && tokens[0] != tokens[1]);
		CHECK(toWeb.empty());
		bridge.RespondTo(tokens[0], nlohmann::json{ { "who", "a" } });
		bridge.RespondTo(tokens[1], nlohmann::json{ { "who", "b" } });
		CHECK(toWeb.size() == 2);
		// Each document gets its OWN answer, under the id it chose.
		CHECK(toWeb[0].first == "view.a/panel" && PayloadOf(Envelope(toWeb, 0)).value("who", "") == "a");
		CHECK(toWeb[1].first == "view.b/panel" && PayloadOf(Envelope(toWeb, 1)).value("who", "") == "b");
		CHECK(Envelope(toWeb, 0).value("id", "") == "q1" && Envelope(toWeb, 1).value("id", "") == "q1");

		// A handler that returns having settled nothing is a PLATFORM bug and
		// the one failure the caller cannot tell from a hang. Answer `internal`
		// and make it loud on both sides.
		bridge.RegisterRequest("test.silent", [](const nlohmann::json&, MessageBridge&) {});
		toWeb.clear();
		bridge.HandleWebMessage("someview", RequestMsg("test.silent", "r6"));
		CHECK(toWeb.size() == 1);
		CHECK(PayloadOf(Last(toWeb)).value("code", "") == "internal");
		CHECK(LoggedContaining("ERROR", "returned without settling"));

		// An unknown endpoint is a correlated error, and warned once per name
		// (pages poll, and a flooded log hides the first occurrence).
		toWeb.clear();
		bridge.HandleWebMessage("someview", RequestMsg("nope.no.such", "r7"));
		CHECK(toWeb.size() == 1);
		CHECK(PayloadOf(Last(toWeb)).value("code", "") == "unknown-endpoint");
		CHECK(Last(toWeb).value("id", "") == "r7");

		// The mirror image of the earlier wrong-endpoint-kind: a send naming a
		// request endpoint is dropped (nothing to answer) and surfaced.
		toWeb.clear();
		g_surfaced.clear();
		bridge.HandleWebMessage("someview", SendMsg("test.reply"));
		CHECK(toWeb.empty());
		CHECK(LastSurfacedCode() == "wrong-endpoint-kind");
		CHECK(!g_surfaced.empty() && g_surfaced.back().detail.value("name", "") == "test.reply");

		// Sends and requests share one first-wins namespace while retaining
		// strict, disjoint routing kinds.
		CHECK(!bridge.RegisterRequest("acme.mymod.ping", [](const nlohmann::json&, MessageBridge&) {}));
		bridge.RegisterSend("test.reply", [](const nlohmann::json&, MessageBridge&) {});
		CHECK(bridge.HasSend("acme.mymod.ping"));
		CHECK(!bridge.HasRequest("acme.mymod.ping"));
		CHECK(bridge.HasRequest("test.reply") && !bridge.HasSend("test.reply"));
		CHECK(LoggedContaining("WARN", "name already registered"));

		// Correlation ids are bounded because the inbound payload is untrusted.
		// An over-long one is a HARD invalid-request: 1.x demoted it to
		// fire-and-forget, which turned a client bug into a request that never
		// settles and a page that waits out its whole timeout.
		toWeb.clear();
		g_surfaced.clear();
		bridge.HandleWebMessage("someview", RequestMsg("test.reply", std::string(65, 'x')));
		CHECK(toWeb.empty());
		CHECK(LastSurfacedCode() == "invalid-request");
		// ...and the boundary value is still accepted.
		bridge.HandleWebMessage("someview", RequestMsg("test.reply", std::string(64, 'x')));
		CHECK(toWeb.size() == 1);
		// A missing or non-string id never dispatches either.
		bridge.HandleWebMessage("someview", R"({"kind":"request","name":"test.reply","payload":{}})");
		bridge.HandleWebMessage("someview", R"({"kind":"request","name":"test.reply","id":7,"payload":{}})");
		CHECK(toWeb.size() == 1);
		CHECK(LastSurfacedCode() == "invalid-request");
	}

	// --- ABI request/response: the plugin side --------------------------------
	api.RegisterRequest("acme.mymod.getWeight", &RequestHandler, nullptr);
	api.RegisterRequest("acme.mymod.getWeight", &RequestHandler, nullptr); // duplicate refused
	api.RegisterRequest("acme.mymod.ping", &RequestHandler, nullptr);      // send/request collision refused
	api.RegisterSend("acme.mymod.getWeight", &HandlerA, nullptr);          // reverse collision refused
	api.PumpMainThread();
	CHECK(LoggedContaining("WARN", "refused RegisterRequest('acme.mymod.getWeight')"));
	CHECK(LoggedContaining("WARN", "refused RegisterRequest('acme.mymod.ping')"));
	CHECK(LoggedContaining("WARN", "refused RegisterSend('acme.mymod.getWeight')"));
	CHECK(bridge.HasRequest("acme.mymod.getWeight"));
	CHECK(!bridge.HasSend("acme.mymod.getWeight"));  // one name, one kind

	// A plugin request is always deferred: the answer arrives whenever the
	// plugin gets there, and PumpMainThread settles it by id.
	toWeb.clear(); g_request.reset();
	bridge.HandleWebMessage("request-view", RequestMsg("acme.mymod.getWeight", "rq1", R"({"unit":"kg"})"));
	CHECK(g_request.has_value()); CHECK(toWeb.empty());
	if (g_request) {
		CHECK(g_requestCommand == "acme.mymod.getWeight");
		CHECK(g_requestSource == "request-view");
		// Verbatim: routing metadata rides the envelope, so there is no
		// injected requestId and no `command` field to strip out first.
		CHECK(g_requestPayload == R"({"unit":"kg"})");
		CHECK(g_requestPayload.find("requestId") == std::string::npos);
		g_request->Respond("acme.mymod.weight", R"({"weight":42.5})");
		g_request->Respond("acme.mymod.weight", R"({"weight":99})"); // ignored
	}
	api.PumpMainThread();
	CHECK(toWeb.size() == 1);
	CHECK(Last(toWeb).value("kind", "") == "reply");
	CHECK(Last(toWeb).value("id", "") == "rq1");
	CHECK(PayloadOf(Last(toWeb)).value("weight", 0.0) == 42.5);
	// The 1.x per-reply `type` has no slot on the 2.0 wire — a reply is
	// correlated by id, and the plugin's type would just be a second, weaker
	// routing channel for the page to get wrong.
	CHECK(!toWeb.empty() && toWeb.back().second.find("acme.mymod.weight") == std::string::npos);
	CHECK(LoggedContaining("WARN", "ignored second response"));

	// A dropped token expires at the host deadline as a correlated error.
	// `no-response` (the backend never answered) stays distinguishable from the
	// helper's client-side `timeout` (the page gave up).
	toWeb.clear(); g_request.reset();
	bridge.HandleWebMessage("request-view", RequestMsg("acme.mymod.getWeight", "rq2"));
	api.PumpMainThread(std::chrono::steady_clock::now() + std::chrono::seconds(31));
	CHECK(toWeb.size() == 1);
	CHECK(Last(toWeb).value("kind", "") == "error");
	CHECK(Last(toWeb).value("id", "") == "rq2");
	CHECK(PayloadOf(Last(toWeb)).value("code", "") == "no-response");

	// Closing the source view reaps its tokens on BOTH sides; a late plugin
	// response is then a safe no-op rather than a write into a dead page.
	toWeb.clear(); g_request.reset();
	bridge.HandleWebMessage("closing-view", RequestMsg("acme.mymod.getWeight", "rq3"));
	api.SetSurfaceLoaded("closing-view", false);
	bridge.OnViewDestroyed("closing-view");
	if (g_request) g_request->Respond(R"({"weight":1})");
	api.PumpMainThread();
	CHECK(toWeb.empty());
	CHECK(LoggedContaining("WARN", "ignored late response"));
	CHECK(LoggedContaining("DEBUG", "reaped in-flight request"));

	// Plugin failures become correlated errors with the plugin's stable code.
	toWeb.clear(); g_request.reset();
	bridge.HandleWebMessage("request-view", RequestMsg("acme.mymod.getWeight", "rq4"));
	if (g_request) g_request->Reject("weight-unavailable", "no player");
	api.PumpMainThread();
	CHECK(toWeb.size() == 1);
	CHECK(PayloadOf(Last(toWeb)).value("code", "") == "weight-unavailable");

	// The 65th in-flight request from one view fails fast instead of growing
	// host memory. Capacity is refused BEFORE dispatch, so a saturated view
	// cannot make the host do the handler's work as well.
	toWeb.clear(); g_requests.clear();
	for (int i = 0; i < 65; ++i) {
		bridge.HandleWebMessage("cap-view", RequestMsg("acme.mymod.getWeight", std::format("cap{}", i)));
	}
	CHECK(g_requests.size() == 64);
	CHECK(toWeb.size() == 1);
	CHECK(Last(toWeb).value("id", "") == "cap64");
	CHECK(PayloadOf(Last(toWeb)).value("code", "") == "request-capacity");
	CHECK(LoggedContaining("WARN", "requests in flight"));
	api.SetSurfaceLoaded("cap-view", false);
	bridge.OnViewDestroyed("cap-view");

	// --- SetViewState: retained state, not a happening -------------------------
	// Retained state replaces view-defined reload messages and manual re-pushes:
	// the plugin
	// sets a value, Runtime retains it, and every fresh document is replayed.
	{
		api.TakeViewStateOps();  // start from an empty queue

		// Validation is synchronous — the caller sees the false at the call.
		CHECK(!api.SetViewState(nullptr, "roster", "{}"));
		CHECK(!api.SetViewState("acme.mymod", nullptr, "{}"));
		CHECK(!api.SetViewState("acme.mymod", "roster", nullptr));
		CHECK(!api.SetViewState("acme.mymod", "", "{}"));
		CHECK(!api.SetViewState("Acme.Mod", "roster", "{}"));
		CHECK(LoggedContaining("WARN", "refused SetViewState('Acme.Mod')"));
		// The key is echoed on the wire and used as a cache key, so it is
		// bounded like every other content-supplied name.
		CHECK(!api.SetViewState("acme.mymod", std::string(129, 'k').c_str(), "{}"));
		CHECK(LoggedContaining("WARN", "key longer than 128 characters"));
		CHECK(!api.SetViewState("acme.mymod", "roster", "{bad"));
		CHECK(LoggedContaining("WARN", "payload is not valid JSON"));
		CHECK(api.TakeViewStateOps().empty());  // nothing invalid was queued

		// Accepted. Unlike a send, state needs no bridge, no loaded surface and
		// no greeting: it is not addressed to a document at all.
		CHECK(api.SetViewState("acme.mymod", "roster", R"({"crew":2})"));
		CHECK(api.SetViewState("osfui", "count", "7"));       // built-in mod id; scalars are values too
		CHECK(api.SetViewState("acme.mymod", "roster", "[1,2]"));
		{
			// FIFO, verbatim: latest-wins is the STORE's job on the main tick,
			// so both writes to the same key must reach it in call order.
			const auto ops = api.TakeViewStateOps();
			CHECK(ops.size() == 3);
			CHECK(ops.size() == 3 && ops[0].mod == "acme.mymod" && ops[0].key == "roster");
			CHECK(ops.size() == 3 && ops[0].value.value("crew", 0) == 2);
			CHECK(ops.size() == 3 && ops[1].mod == "osfui" && ops[1].key == "count" && ops[1].value == 7);
			CHECK(ops.size() == 3 && ops[2].key == "roster" && ops[2].value.is_array());
			CHECK(api.TakeViewStateOps().empty());  // drained
		}

		// The drain belongs to Runtime (which owns ViewStateStore and the
		// replay), so the API pump must not eat these on its way past.
		CHECK(api.SetViewState("acme.mymod", "kept", "{}"));
		api.PumpMainThread();
		CHECK(api.TakeViewStateOps().size() == 1);

		// A producer looping off-thread hits the cap instead of growing memory;
		// the refusal is the INCOMING write, so earlier state is kept.
		for (int i = 0; i < 300; ++i) {
			api.SetViewState("acme.mymod", ("k" + std::to_string(i)).c_str(), "{}");
		}
		const auto capped = api.TakeViewStateOps();
		CHECK(capped.size() == 256);
		CHECK(!capped.empty() && capped.front().key == "k0");  // oldest kept
		CHECK(LoggedContaining("WARN", "pending SetViewState queue full"));
	}

	// --- refused ABI-major callers, made visible -------------------------------
	// OSFUI_RequestBridge refuses ABI 1.x and records the caller so Runtime
	// can raise ONE `compat.legacy-api` card naming the DLL the player has to
	// update, instead of the refusal living only in a log nobody opens.
	{
		api.TakeLegacyApiCallers();  // start from an empty ledger
		api.NoteLegacyApiCaller("OldMod.dll", 1, 8);
		api.NoteLegacyApiCaller("OldMod.dll", 1, 8);  // a plugin retrying every load screen
		api.NoteLegacyApiCaller("", 3, 5);              // unresolvable module: still one card
		{
			const auto callers = api.TakeLegacyApiCallers();
			CHECK(callers.size() == 2);  // deduped by module
			CHECK(callers.size() == 2 && callers[0].module == "OldMod.dll");
			CHECK(callers.size() == 2 && callers[0].major == 1 && callers[0].minor == 8);
			CHECK(callers.size() == 2 && callers[1].module.empty() && callers[1].major == 3);
		}
		CHECK(api.TakeLegacyApiCallers().empty());  // drained
		// Bounded: a load order full of stale plugins cannot grow this.
		for (int i = 0; i < 40; ++i) {
			api.NoteLegacyApiCaller(std::format("mod{}.dll", i), 1, 8);
		}
		CHECK(api.TakeLegacyApiCallers().size() == 32);
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

	// Runtime's real order for a lazily loaded surface: the renderer view is
	// created (SetSurfaceLoaded + OnViewCreated arm a CLOSED gate), the ABI
	// holdback flushes on the next pump, and the document greets whenever it
	// finishes loading.
	lazyBridge.OnViewCreated("acme.mymod/dash");
	api.SetSurfaceLoaded("acme.mymod/dash", true);
	api.PumpMainThread();
	CHECK(toWeb.empty());  // a loaded surface is not a greeted document
	lazyBridge.HandleWebMessage("acme.mymod/dash", kHello);
	// THE DELIVERY GUARANTEE, end to end. The plugin sent before this document
	// existed; the gate held the event; the greeting replays `ready` and then
	// hands it over. That is what Feature::kDeliveryGuarantee promises
	// (RegisterView -> SendToWeb -> RequestMenu in one tick lands before first
	// visible paint), and it is the reason the gate does not discard its
	// backlog on a FIRST greeting — only OnViewCreated clears a queue.
	CHECK(toWeb.size() == 2);
	CHECK(Envelope(toWeb, 0).value("kind", "") == "ready");
	CHECK(Envelope(toWeb, 1).value("kind", "") == "event");
	CHECK(Envelope(toWeb, 1).value("name", "") == "acme.mymod.data");
	CHECK(lazyBridge.HasGreeted("acme.mymod/dash"));

	// Once greeted, a send flushes to the wire on the next pump.
	toWeb.clear();
	CHECK(api.SendToWeb("acme.mymod/dash", "acme.mymod.data", R"({"lazy":true})"));
	api.PumpMainThread();
	CHECK(toWeb.size() == 1);
	CHECK(!toWeb.empty() && toWeb[0].first == "acme.mymod/dash");
	CHECK(Last(toWeb).value("kind", "") == "event");
	CHECK(PayloadOf(Last(toWeb)).value("lazy", false));

	// The send registry re-applies to a replacement bridge, handlers intact.
	lazyBridge.HandleWebMessage("acme.mymod/dash", SendMsg("acme.mymod.catalog.get"));
	CHECK(g_firedA.size() == 3);
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
	// (State-LIKE: real retained state belongs in SetViewState above — this is
	// what a plugin misusing events for state gets, bounded rather than fatal.)
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
	CHECK(toWeb.size() == 64);  // the view greeted earlier: the gate is open
	if (toWeb.size() == 64) {
		const auto first = Envelope(toWeb, 0);
		const auto last = Last(toWeb);
		CHECK(PayloadOf(first).value("seq", -1) == 1);
		CHECK(PayloadOf(last).value("seq", -1) == 64);
	}
	toWeb.clear();
	CHECK(api.SendToWeb("acme.mymod/missing", "acme.mymod.state", R"({"x":1})"));
	api.PumpMainThread();
	CHECK(toWeb.empty());
	api.OnBridgeReady(nullptr);
	api.PumpMainThread();

	// World-surface pattern: Runtime::Initialize marks the instance's view
	// loaded itself right after the catalog (those pages never pass through
	// LoadSurface), so a send to it flushes on the next pump instead of sitting
	// in the holdback forever.
	api.SetViewCatalog({ "acme.mymod/dash", "osfui/settings", "acme.mymod/worldpanel" });
	api.SetSurfaceLoaded("acme.mymod/worldpanel", true);
	api.OnBridgeReady(&lazyBridge);
	lazyBridge.OnViewCreated("acme.mymod/worldpanel");
	lazyBridge.HandleWebMessage("acme.mymod/worldpanel", kHello);
	toWeb.clear();
	CHECK(api.SendToWeb("acme.mymod/worldpanel", "acme.mymod.state", R"({"world":1})"));
	api.PumpMainThread();
	CHECK(toWeb.size() == 1 && toWeb[0].first == "acme.mymod/worldpanel");
	CHECK(Last(toWeb).value("kind", "") == "event");
	api.OnBridgeReady(nullptr);
	api.PumpMainThread();

	// --- the Client wrapper (item 4): version-gated calls ---------------------
	{
		using OSFUI::API::Client;
		using OSFUI::API::Feature;

		Client c;
		TestHost testHost;
		CHECK(c.Attach(&testHost));
		CHECK(c.Has(Feature::kDiagnostics));
		CHECK(c.Has(Feature::kRequests));
		CHECK(c.Has(Feature::kViewState));
		CHECK(!c.SetViewState("acme.mymod", "k", "{}"));  // test double returns false
		c.RegisterRequest("acme.mymod.old", &RequestHandler, nullptr);
		c.UnregisterRequest("acme.mymod.old");
		CHECK(testHost.requestCalls == 2);
		c.Attach(nullptr);
		CHECK(!c.IsConnected());
		CHECK(!c.Has(Feature::kSends));              // unattached: everything gates off
		CHECK(!c.RequestMenu("osfui/settings", true));
		CHECK(!c.SetViewState("acme.mymod", "k", "{}"));
		CHECK(c.GetSettingString("a.b", "k", nullptr, 0) == 0);
		CHECK(c.GetBridgeProtocolVersion() != nullptr);  // "" — never a null crash

		CHECK(c.Attach(&api));
		CHECK(c.IsConnected() && static_cast<bool>(c));
		CHECK(c.GetInterfaceVersion() == OSFUI::API::kBridgeAPIVersion);
		CHECK(c.Raw() == &api);
		CHECK(c.Has(Feature::kSends));
		CHECK(c.Has(Feature::kViewState));

		// Ungated pass-throughs reach the real implementation.
		CHECK(c.SetViewState("acme.mymod", "wrapper", R"({"via":"client"})"));
		{
			const auto ops = api.TakeViewStateOps();
			CHECK(ops.size() == 1 && ops[0].key == "wrapper");
		}

		// Every currently shipped feature is part of the 2.0 baseline.
		CHECK(c.Has(Feature::kRegisterView));
		CHECK(c.Has(Feature::kRequestMenu));
		CHECK(c.Has(Feature::kSettings));
		CHECK(c.Has(Feature::kHotkeys));
		CHECK(c.RegisterView("acme.mymod/extra"));   // reaches the host, not gated off
		CHECK(!c.RegisterView("unqualified"));       // refused on its own merits
		{
			const auto regs = api.TakeViewRegistrations();
			CHECK(regs.size() == 1 && regs[0] == "acme.mymod/extra");
		}
		c.Attach(nullptr);
		CHECK(!c.IsConnected());
	}

	// --- health reporting (System Health) -------------------------------------
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
		using OSFUI::API::JsonSend;
		using OSFUI::API::JsonRequest;

		JsonSend send{ "acme.mymod.set", R"({"weight":42.5,"name":"ore"})", "acme.mymod/view" };
		CHECK(send.IsValid());
		CHECK(send.Name() == "acme.mymod.set");
		CHECK(send.SourceViewId() == "acme.mymod/view");
		CHECK(send.Require<double>("weight") == 42.5);
		CHECK(send.Value<std::string>("name", "") == "ore");
		CHECK(send.Value<int>("missing", 7) == 7);
		int numeric = 0;
		CHECK(!send.TryGet("name", numeric));  // wrong field type returns false

		JsonSend malformed{ "x", "{bad", "v" };
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
