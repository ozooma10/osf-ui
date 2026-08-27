
#include "API/BridgeApi.h"

#include "Core/Log.h"
#include "Core/Version.h"  // kOsfuiReleaseVersion / kBridgeProtocolVersion (the `ready` payload)
#include "Bridge/MessageBridge.h"
#include "check.h"

namespace
{

	bool LoggedContaining(std::string_view a_level, std::string_view a_needle)
	{
		for (const auto& entry : REX::test::Entries()) {
			if (entry.starts_with(a_level) && entry.find(a_needle) != std::string::npos) {
				return true;
			}
		}
		return false;
	}

	constexpr const char* kHello = R"({"kind":"send","name":"osfui.hello","payload":{}})";

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
	struct RelativeFire
	{
		std::string view;
		OSFUI::API::Views::RelativePointerPhase phase;
		float dx;
		float dy;
		float wheel;
	};
	std::vector<RelativeFire> g_relativeFires;
	void RelativePointer(const char* a_view, OSFUI::API::Views::RelativePointerPhase a_phase,
		float a_dx, float a_dy, float a_wheel, void*) noexcept
	{
		g_relativeFires.push_back({ a_view, a_phase, a_dx, a_dy, a_wheel });
	}
	std::vector<std::string> g_viewOpenPreflightFires;
	bool ViewOpenPreflight(const char* a_view, void* a_user) noexcept
	{
		g_viewOpenPreflightFires.emplace_back(a_view);
		return *static_cast<const bool*>(a_user);
	}
	struct ViewLifecycleFire
	{
		std::string view;
		OSFUI::API::Views::ViewLifecyclePhase phase;
		void* user;
	};
	std::vector<ViewLifecycleFire> g_viewLifecycleFires;
	void ViewLifecycle(const char* a_view, OSFUI::API::Views::ViewLifecyclePhase a_phase, void* a_user) noexcept
	{
		g_viewLifecycleFires.push_back({ a_view, a_phase, a_user });
	}

	struct ProtocolFault
	{
		std::string    view;
		std::string    code;
		std::string    message;
		nlohmann::json detail;
	};
	std::vector<ProtocolFault> g_protocolFaults;
	std::string LastProtocolFaultCode()
	{
		return g_protocolFaults.empty() ? std::string{} : g_protocolFaults.back().code;
	}

	std::optional<OSFUI::API::Views::Request> g_request;
	std::vector<OSFUI::API::Views::Request> g_requests;
	std::string g_requestCommand, g_requestPayload, g_requestSource;
	void RequestHandler(const OSFUI::API::Views::Request& a_request, void*) noexcept
	{
		g_request = a_request;
		g_requests.push_back(a_request);
		g_requestCommand = a_request.name; g_requestPayload = a_request.payloadJson; g_requestSource = a_request.sourceViewId;
	}

	struct TestServices final :
		OSFUI::API::Settings::ISettings,
		OSFUI::API::Views::IViews,
		OSFUI::API::Diagnostics::IDiagnostics
	{
		int sendCalls{ 0 };
		int requestCalls{ 0 };
		int relativePointerCalls{ 0 };
		int viewOpenPreflightCalls{ 0 };
		int viewLifecycleCalls{ 0 };
		bool viewOpenPreflightResult{ true };
		bool viewLifecycleResult{ true };
		bool IsReady() override { return false; }
		void RegisterSend(const char*, OSFUI::API::Views::SendFn, void*) override { ++sendCalls; }
		void UnregisterSend(const char*) override { ++sendCalls; }
		bool RegisterRelativePointer(const char*, OSFUI::API::Views::RelativePointerFn, void*) override { ++relativePointerCalls; return true; }
		void UnregisterRelativePointer(const char*) override { ++relativePointerCalls; }
		bool RegisterViewOpenPreflight(const char*, OSFUI::API::Views::ViewOpenPreflightFn, void*) override { ++viewOpenPreflightCalls; return viewOpenPreflightResult; }
		void UnregisterViewOpenPreflight(const char*) override { ++viewOpenPreflightCalls; }
		bool RegisterViewLifecycle(const char*, OSFUI::API::Views::ViewLifecycleFn, void*) override { ++viewLifecycleCalls; return viewLifecycleResult; }
		void UnregisterViewLifecycle(const char*) override { ++viewLifecycleCalls; }
		bool SendToWeb(const char*, const char*, const char*) override { return false; }
		bool SetViewState(const char*, const char*, const char*) override { return false; }
		void SetReadyCallback(OSFUI::API::Views::ReadyFn, void*) override {}
		bool RequestMenu(const char*, bool) override { return false; }
		std::uint32_t SubscribeSettings(const char*, OSFUI::API::Settings::SettingChangedFn, void*) override { return 0; }
		void UnsubscribeSettings(std::uint32_t) override {}
		bool GetSettingBool(const char*, const char*, bool*) override { return false; }
		bool GetSettingInt(const char*, const char*, std::int64_t*) override { return false; }
		bool GetSettingFloat(const char*, const char*, double*) override { return false; }
		std::uint32_t GetSettingString(const char*, const char*, char*, std::uint32_t) override { return 0; }
		std::uint32_t SubscribeHotkey(const char*, const char*, OSFUI::API::Settings::HotkeyFn, void*) override { return 0; }
		void UnsubscribeHotkey(std::uint32_t) override {}
		bool RegisterView(const char*) override { return false; }
		bool ReportIssue(const char*, const char*, const char*, std::uint32_t, const char*, const char*) override { return false; }
		bool ClearIssue(const char*, const char*) override { return false; }
		bool ClearIssuesExcept(const char*, const char*) override { return false; }
		void RegisterRequest(const char*, OSFUI::API::Views::RequestFn, void*) override { ++requestCalls; }
		void UnregisterRequest(const char*) override { ++requestCalls; }
	};
}

// Core/Log.h declarations (real impl pulls game deps — stub it here).
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
	using OSFUI::MessageBridge;
	auto& api = OSFUI::API::BridgeApi::Get();
	api.SetViewCatalog({ "someview" });
	api.SetViewInstantiated("someview", true);

	// --- standalone service versions ------------------------------------------
	CHECK(OSFUI::API::kBridgeAPIVersion == ((1u << 16) | 7u));
	CHECK(OSFUI::API::Settings::kVersion == ((1u << 16) | 0u));
	CHECK(OSFUI::API::Views::kVersion == ((1u << 16) | 0u));
	CHECK(OSFUI::API::Diagnostics::kVersion == ((1u << 16) | 0u));
	CHECK(static_cast<std::uint32_t>(OSFUI::API::Views::ViewLifecyclePhase::kShown) == 0);
	CHECK(static_cast<std::uint32_t>(OSFUI::API::Views::ViewLifecyclePhase::kFrame) == 1);
	CHECK(static_cast<std::uint32_t>(OSFUI::API::Views::ViewLifecyclePhase::kHidden) == 2);
	CHECK(std::string_view(OSFUI::kBridgeProtocolVersion) == "2.0");
	CHECK(std::string_view(api.GetBridgeProtocolVersion()) == "2.0");

	// --- relative pointer ownership: exact view, first-wins, main-thread dispatch ---
	CHECK(!api.RegisterRelativePointer("unqualified", &RelativePointer, nullptr));
	CHECK(api.RegisterRelativePointer("acme.mymod/panel", &RelativePointer, nullptr));
	CHECK(!api.RegisterRelativePointer("acme.mymod/panel", &RelativePointer, nullptr));
	CHECK(api.HasRelativePointer("acme.mymod/panel"));
	CHECK(api.DispatchRelativePointer("acme.mymod/panel", OSFUI::API::Views::RelativePointerPhase::kBegin));
	CHECK(api.DispatchRelativePointer("acme.mymod/panel", OSFUI::API::Views::RelativePointerPhase::kUpdate, 4.0f, -2.0f, 1.0f));
	CHECK(g_relativeFires.size() == 2);
	CHECK(g_relativeFires.back().view == "acme.mymod/panel");
	CHECK(g_relativeFires.back().phase == OSFUI::API::Views::RelativePointerPhase::kUpdate);
	CHECK(g_relativeFires.back().dx == 4.0f && g_relativeFires.back().dy == -2.0f && g_relativeFires.back().wheel == 1.0f);
	api.UnregisterRelativePointer("acme.mymod/panel");
	CHECK(!api.HasRelativePointer("acme.mymod/panel"));
	CHECK(!api.DispatchRelativePointer("acme.mymod/panel", OSFUI::API::Views::RelativePointerPhase::kCancel));

	// --- view-open preflight ownership: validation, first-wins, allow/deny, re-register ---
	bool allow = true;
	bool deny = false;
	CHECK(!api.RegisterViewOpenPreflight("unqualified", &ViewOpenPreflight, &allow));
	CHECK(!api.RegisterViewOpenPreflight("acme.mymod/panel", nullptr, nullptr));
	CHECK(api.RegisterViewOpenPreflight("acme.mymod/panel", &ViewOpenPreflight, &allow));
	CHECK(!api.RegisterViewOpenPreflight("acme.mymod/panel", &ViewOpenPreflight, &deny));
	CHECK(api.RunViewOpenPreflight("acme.mymod/missing") == OSFUI::API::BridgeApi::ViewOpenPreflightResult::kNoHandler);
	CHECK(api.RunViewOpenPreflight("acme.mymod/panel") == OSFUI::API::BridgeApi::ViewOpenPreflightResult::kAllowed);
	CHECK(g_viewOpenPreflightFires.size() == 1 && g_viewOpenPreflightFires.back() == "acme.mymod/panel");
	api.UnregisterViewOpenPreflight("acme.mymod/panel");
	CHECK(api.RunViewOpenPreflight("acme.mymod/panel") == OSFUI::API::BridgeApi::ViewOpenPreflightResult::kNoHandler);
	CHECK(api.RegisterViewOpenPreflight("acme.mymod/panel", &ViewOpenPreflight, &deny));
	CHECK(api.RunViewOpenPreflight("acme.mymod/panel") == OSFUI::API::BridgeApi::ViewOpenPreflightResult::kDenied);
	api.UnregisterViewOpenPreflight("acme.mymod/panel");

	// --- view lifecycle ownership: exact view, first-wins, explicit phases ---
	int lifecycleOwner = 7;
	CHECK(!api.RegisterViewLifecycle("unqualified", &ViewLifecycle, &lifecycleOwner));
	CHECK(!api.RegisterViewLifecycle("acme.mymod/panel", nullptr, nullptr));
	CHECK(api.RegisterViewLifecycle("acme.mymod/panel", &ViewLifecycle, &lifecycleOwner));
	CHECK(!api.RegisterViewLifecycle("acme.mymod/panel", &ViewLifecycle, nullptr));
	CHECK(!api.DispatchViewLifecycle("ACME.MYMOD/panel", OSFUI::API::Views::ViewLifecyclePhase::kShown));
	CHECK(api.DispatchViewLifecycle("acme.mymod/panel", OSFUI::API::Views::ViewLifecyclePhase::kShown));
	CHECK(api.DispatchViewLifecycle("acme.mymod/panel", OSFUI::API::Views::ViewLifecyclePhase::kFrame));
	CHECK(api.DispatchViewLifecycle("acme.mymod/panel", OSFUI::API::Views::ViewLifecyclePhase::kHidden));
	CHECK(g_viewLifecycleFires.size() == 3);
	CHECK(g_viewLifecycleFires[0].view == "acme.mymod/panel");
	CHECK(g_viewLifecycleFires[0].phase == OSFUI::API::Views::ViewLifecyclePhase::kShown);
	CHECK(g_viewLifecycleFires[1].phase == OSFUI::API::Views::ViewLifecyclePhase::kFrame);
	CHECK(g_viewLifecycleFires[2].phase == OSFUI::API::Views::ViewLifecyclePhase::kHidden);
	CHECK(g_viewLifecycleFires[2].user == &lifecycleOwner);
	api.UnregisterViewLifecycle("acme.mymod/panel");
	CHECK(!api.DispatchViewLifecycle("acme.mymod/panel", OSFUI::API::Views::ViewLifecyclePhase::kFrame));

	// --- endpoint ownership: explicit, with no dot-count inference -------------
	for (const auto* bad : { "close", "ping", "setVisible", "menu.open",
	                         "settings.set", "papyrus.call", "osfui.hello",
	                         "osfui.gamepadMode", "osfui.gamepadRaw", "OSFUI.private" }) {
		api.RegisterSend(bad, &HandlerA, nullptr);
	}
	CHECK(LoggedContaining("WARN", "refused RegisterSend('close')"));
	CHECK(LoggedContaining("WARN", "refused RegisterSend('menu.open')"));
	CHECK(LoggedContaining("WARN", "refused RegisterSend('osfui.hello')"));
	CHECK(LoggedContaining("WARN", "refused RegisterSend('OSFUI.private')"));
	api.RegisterSend("papyrus.send", &HandlerA, nullptr);
	CHECK(!LoggedContaining("WARN", "refused RegisterSend('papyrus.send')"));
	api.UnregisterSend("papyrus.send");
	api.RegisterRequest("papyrus.request", &RequestHandler, nullptr);
	CHECK(!LoggedContaining("WARN", "refused RegisterRequest('papyrus.request')"));
	api.UnregisterRequest("papyrus.request");

	// Opaque plugin endpoint names need no particular dot count.
	api.RegisterSend("acme.mymod.ping", &HandlerA, nullptr);
	api.RegisterSend("acme.mymod.catalog.get", &HandlerA, nullptr);
	api.RegisterSend("plainmod.ping", &HandlerA, nullptr);
	api.RegisterSend("Ship Status!", &HandlerA, nullptr);

	// --- duplicates: first-wins, refused (item 3) -----------------------------
	api.RegisterSend("acme.mymod.ping", &HandlerB, nullptr);  // hijack attempt
	CHECK(LoggedContaining("WARN", "refused RegisterSend('acme.mymod.ping') — already registered"));

	// --- apply to a live bridge -----------------------------------------------
	Outbox toWeb;
	MessageBridge bridge([&](std::string_view a_viewId, std::string_view a_json) {
		toWeb.emplace_back(std::string(a_viewId), std::string(a_json));
	});
	bridge.SetProtocolFaultSink([](std::string_view a_view, std::string_view a_code,
						   std::string_view a_message, const nlohmann::json& a_detail, bool) {
		g_protocolFaults.push_back({ std::string(a_view), std::string(a_code), std::string(a_message), a_detail });
	});
	api.SetBridgeAvailability(&bridge);
	api.PumpMainThread();

	bridge.OnViewCreated("someview");
	toWeb.clear();
	bridge.HandleWebMessage("someview", kHello);
	CHECK(toWeb.size() == 1);
	{
		const auto ready = Last(toWeb);
		const auto info = PayloadOf(ready);
		CHECK(ready.value("kind", "") == "ready");
		CHECK(info.value("game", "") == "Starfield");
		CHECK(info.value("plugin", "") == OSFUI::kPluginName);
		CHECK(info.value("version", "") == OSFUI::kOsfuiReleaseVersion);
		CHECK(info.value("bridgeVersion", "") == OSFUI::kBridgeProtocolVersion);
		CHECK(info.value("view", "") == "someview");
		CHECK(info.value("mod", "") == "someview");
		CHECK(!info.contains("capabilities"));  // removed pre-1.0, still gone
	}

	// Local JS names resolve through the authoritative source-mod namespace.
	// Exact/native routing stays ahead of the Papyrus-style fallback, while a
	// fully-qualified name remains available to a different view.
	{
		Outbox routeOut;
		std::vector<std::string> routed;
		std::vector<ProtocolFault> faults;
		int fallbackProbes = 0;
		MessageBridge routes([&](std::string_view a_view, std::string_view a_json) {
			routeOut.emplace_back(a_view, a_json);
		});
		routes.SetProtocolFaultSink([&](std::string_view a_view, std::string_view a_code,
			std::string_view a_message, const nlohmann::json& a_detail, bool) {
			faults.push_back({ std::string(a_view), std::string(a_code), std::string(a_message), a_detail });
		});
		routes.RegisterSend("acme.route.fire", [&](const nlohmann::json&, MessageBridge& a_b) {
			routed.push_back("native-send:" + std::string(a_b.CurrentSource()));
		});
		routes.RegisterRequest("acme.route.ask", [&](const nlohmann::json&, MessageBridge& a_b) {
			routed.push_back("native-request:" + std::string(a_b.CurrentSource()));
			a_b.Respond(nlohmann::json{ { "route", "native" } });
		});
		routes.SetEndpointFallback(
			[&](std::string_view a_source, std::string_view a_name) {
				++fallbackProbes;
				// Deliberately claim the opposite Papyrus kind. Owner-qualified
				// native endpoints must still win before this probe is consulted.
				if (a_source.starts_with("acme.route/") && a_name == "fire") {
					return MessageBridge::FallbackEndpointKind::kRequest;
				}
				if (a_source.starts_with("acme.route/") && a_name == "ask") {
					return MessageBridge::FallbackEndpointKind::kSend;
				}
				const bool owns = a_source.starts_with("acme.paper/");
				if ((owns && a_name == "papSend") || a_name == "acme.paper.papSend") {
					return MessageBridge::FallbackEndpointKind::kSend;
				}
				if ((owns && a_name == "papRequest") || a_name == "acme.paper.papRequest") {
					return MessageBridge::FallbackEndpointKind::kRequest;
				}
				return MessageBridge::FallbackEndpointKind::kNone;
			},
			[&](std::string_view a_name, const nlohmann::json&, MessageBridge& a_b) {
				routed.push_back("papyrus-send:" + std::string(a_name) + ":" + std::string(a_b.CurrentSource()));
			},
			[&](std::string_view a_name, const nlohmann::json&, MessageBridge& a_b) {
				routed.push_back("papyrus-request:" + std::string(a_name) + ":" + std::string(a_b.CurrentSource()));
				a_b.Respond(17);
			});

		routes.HandleWebMessage("acme.route/panel", SendMsg("fire"));
		CHECK(routed.size() == 1 && routed.back() == "native-send:acme.route/panel");
		CHECK(fallbackProbes == 0);  // owner-qualified native alias beats fallback
		routes.HandleWebMessage("other.mod/panel", SendMsg("acme.route.fire"));
		CHECK(routed.size() == 2 && routed.back() == "native-send:other.mod/panel");

		routeOut.clear();
		routes.HandleWebMessage("acme.route/panel", RequestMsg("ask", "native-r"));
		CHECK(routed.size() == 3 && routed.back() == "native-request:acme.route/panel");
		CHECK(routeOut.size() == 1 && PayloadOf(Last(routeOut)).value("route", "") == "native");

		faults.clear();
		routes.HandleWebMessage("acme.route/panel", SendMsg("ask"));
		CHECK(faults.size() == 1 && faults.back().code == "wrong-endpoint-kind");
		routeOut.clear();
		routes.HandleWebMessage("acme.route/panel", RequestMsg("fire", "wrong-native"));
		CHECK(routeOut.size() == 1 && PayloadOf(Last(routeOut)).value("code", "") == "wrong-endpoint-kind");
		CHECK(fallbackProbes == 0);

		routes.HandleWebMessage("acme.paper/panel", SendMsg("papSend"));
		CHECK(routed.size() == 4 && routed.back().starts_with("papyrus-send:papSend:"));
		routes.HandleWebMessage("osfui/settings", SendMsg("acme.paper.papSend"));
		CHECK(routed.size() == 5 && routed.back() == "papyrus-send:acme.paper.papSend:osfui/settings");
		routeOut.clear();
		routes.HandleWebMessage("acme.paper/panel", RequestMsg("papRequest", "paper-r"));
		CHECK(routed.size() == 6 && routed.back().starts_with("papyrus-request:papRequest:"));
		CHECK(routeOut.size() == 1 && PayloadOf(Last(routeOut)) == 17);

		faults.clear();
		routes.HandleWebMessage("acme.paper/panel", SendMsg("papRequest"));
		CHECK(faults.size() == 1 && faults.back().code == "wrong-endpoint-kind");
		routeOut.clear();
		routes.HandleWebMessage("acme.paper/panel", RequestMsg("papSend", "wrong-paper"));
		CHECK(routeOut.size() == 1 && PayloadOf(Last(routeOut)).value("code", "") == "wrong-endpoint-kind");

		faults.clear();
		routes.HandleWebMessage("other.mod/panel", SendMsg("papSend"));
		CHECK(faults.size() == 1 && faults.back().code == "unknown-endpoint");
	}

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

	toWeb.clear();
	bridge.HandleWebMessage("someview", SendMsg("acme.mymod.ping", R"({"x":1})"));
	CHECK(g_firedA.size() == 1);
	CHECK(g_firedB.empty());  // the duplicate registration never took
	CHECK(LoggedContaining("TRACE", "'acme.mymod.ping' from view 'someview'"));
	CHECK(!LoggedContaining("DEBUG", "'acme.mymod.ping' from view 'someview'"));
	if (!g_firedA.empty()) {
		CHECK(g_firedA[0].command == "acme.mymod.ping");
		CHECK(g_firedA[0].source == "someview");
		CHECK(g_firedA[0].payload == R"({"x":1})");
		CHECK(g_firedA[0].payload.find("requestId") == std::string::npos);
	}
	CHECK(toWeb.empty());  // no auto-ack

	// An explicitly empty payload is delivered as an object.
	bridge.HandleWebMessage("someview", SendMsg("acme.mymod.catalog.get", "{}"));
	CHECK(g_firedA.size() == 2);
	CHECK(g_firedA.back().payload == "{}");
	CHECK(toWeb.empty());
	g_protocolFaults.clear();
	bridge.HandleWebMessage("someview", R"({"kind":"send","name":"acme.mymod.ping"})");
	bridge.HandleWebMessage("someview", R"({"kind":"send","name":"acme.mymod.ping","payload":null})");
	CHECK(g_firedA.size() == 2);
	CHECK(g_protocolFaults.size() == 2);
	CHECK(LastProtocolFaultCode() == "invalid-request");

	g_protocolFaults.clear();
	bridge.HandleWebMessage("someview", SendMsg("close"));
	CHECK(g_firedA.size() == 2);
	CHECK(toWeb.empty());
	CHECK(LastProtocolFaultCode() == "unknown-endpoint");
	CHECK(LoggedContaining("WARN", "dropped send to unknown endpoint 'close'"));

	// --- envelope hygiene: routing metadata is structural, not advisory -------
	{
		g_protocolFaults.clear();
		bridge.HandleWebMessage("someview",
			R"({"kind":"send","name":"acme.mymod.ping","id":"x","payload":{}})");
		CHECK(g_firedA.size() == 2);
		CHECK(toWeb.empty());
		CHECK(LastProtocolFaultCode() == "invalid-request");

		bridge.HandleWebMessage("someview",
			R"({"kind":"send","name":"acme.mymod.ping","payload":[1,2]})");
		CHECK(g_firedA.size() == 2);
		CHECK(LastProtocolFaultCode() == "invalid-request");

		bridge.HandleWebMessage("someview",
			R"({"type":"ui.command","payload":{"command":"acme.mymod.ping"}})");
		CHECK(g_firedA.size() == 2);
		CHECK(LastProtocolFaultCode() == "invalid-request");

		bridge.HandleWebMessage("someview", R"({"kind":"send","name":""})");
		CHECK(LastProtocolFaultCode() == "invalid-request");
		bridge.HandleWebMessage("someview", R"({"name":"acme.mymod.ping","payload":{}})");
		bridge.HandleWebMessage("someview", R"({"kind":"send","name":7,"payload":{}})");
		bridge.HandleWebMessage("someview",
			nlohmann::json{ { "kind", "send" }, { "name", std::string(129, 'x') },
				{ "payload", nlohmann::json::object() } }.dump());
		bridge.HandleWebMessage("someview",
			R"({"kind":"send","name":"acme.mymod.ping","id":null,"payload":{}})");
		CHECK(g_firedA.size() == 2);
		CHECK(LastProtocolFaultCode() == "invalid-request");

		toWeb.clear();
		bridge.HandleWebMessage("someview", "not json at all");
		CHECK(toWeb.empty());
		CHECK(LastProtocolFaultCode() == "invalid-request");
	}

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

		bridge.RegisterRequest("test.twice", [](const nlohmann::json&, MessageBridge& a_b) {
			a_b.Respond(nlohmann::json::object());
			a_b.Respond(nlohmann::json{ { "second", true } });
		});
		toWeb.clear();
		bridge.HandleWebMessage("someview", RequestMsg("test.twice", "r3b"));
		CHECK(toWeb.size() == 1);
		CHECK(LoggedContaining("WARN", "settled twice"));

		std::string deferToken;
		bridge.RegisterRequest("test.defer", [&](const nlohmann::json&, MessageBridge& a_b) {
			deferToken = a_b.Defer();
		});
		toWeb.clear();
		bridge.HandleWebMessage("someview", RequestMsg("test.defer", "r4"));
		CHECK(toWeb.empty());
		CHECK(!deferToken.empty() && deferToken != "r4");
		bridge.RespondTo(deferToken, nlohmann::json{ { "done", true } });
		CHECK(toWeb.size() == 1);
		CHECK(Last(toWeb).value("kind", "") == "reply");
		CHECK(Last(toWeb).value("id", "") == "r4");
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

		bridge.RegisterRequest("test.silent", [](const nlohmann::json&, MessageBridge&) {});
		toWeb.clear();
		bridge.HandleWebMessage("someview", RequestMsg("test.silent", "r6"));
		CHECK(toWeb.size() == 1);
		CHECK(PayloadOf(Last(toWeb)).value("code", "") == "internal");
		CHECK(LoggedContaining("ERROR", "returned without settling"));

		bridge.RegisterRequest("test.throw", [](const nlohmann::json&, MessageBridge&) {
			throw std::runtime_error("handler failed");
		});
		toWeb.clear();
		bool threw = false;
		try {
			bridge.HandleWebMessage("throwing-view", RequestMsg("test.throw", "r-throw"));
		} catch (const std::runtime_error&) {
			threw = true;
		}
		CHECK(threw);
		bridge.Respond(nlohmann::json{ { "stale", true } });
		CHECK(toWeb.empty());
		bridge.HandleWebMessage("someview", RequestMsg("test.reply", "r-after-throw"));
		CHECK(toWeb.size() == 1);
		CHECK(Last(toWeb).value("id", "") == "r-after-throw");

		toWeb.clear();
		bridge.HandleWebMessage("someview", RequestMsg("nope.no.such", "r7"));
		CHECK(toWeb.size() == 1);
		CHECK(PayloadOf(Last(toWeb)).value("code", "") == "unknown-endpoint");
		CHECK(Last(toWeb).value("id", "") == "r7");

		toWeb.clear();
		g_protocolFaults.clear();
		bridge.HandleWebMessage("someview", SendMsg("test.reply"));
		CHECK(toWeb.empty());
		CHECK(LastProtocolFaultCode() == "wrong-endpoint-kind");
		CHECK(!g_protocolFaults.empty() && g_protocolFaults.back().detail.value("name", "") == "test.reply");

		CHECK(!bridge.RegisterRequest("acme.mymod.ping", [](const nlohmann::json&, MessageBridge&) {}));
		bridge.RegisterSend("test.reply", [](const nlohmann::json&, MessageBridge&) {});
		CHECK(LoggedContaining("WARN", "name already registered"));
		const auto firedBefore = g_firedB.size();
		toWeb.clear();
		bridge.HandleWebMessage("someview", SendMsg("acme.mymod.ping"));
		CHECK(g_firedB.size() == firedBefore + 1);
		bridge.HandleWebMessage("someview", RequestMsg("acme.mymod.ping", "kind-send"));
		CHECK(g_firedB.size() == firedBefore + 1);
		CHECK(PayloadOf(Last(toWeb)).value("code", "") == "wrong-endpoint-kind");
		toWeb.clear();
		bridge.HandleWebMessage("someview", RequestMsg("test.reply", "kind-request"));
		CHECK(toWeb.size() == 1 && Last(toWeb).value("kind", "") == "reply");
		g_protocolFaults.clear();
		toWeb.clear();
		bridge.HandleWebMessage("someview", SendMsg("test.reply"));
		CHECK(toWeb.empty());
		CHECK(LastProtocolFaultCode() == "wrong-endpoint-kind");

		toWeb.clear();
		g_protocolFaults.clear();
		bridge.HandleWebMessage("someview", RequestMsg("test.reply", std::string(65, 'x')));
		CHECK(toWeb.empty());
		CHECK(LastProtocolFaultCode() == "invalid-request");
		// ...and the boundary value is still accepted.
		bridge.HandleWebMessage("someview", RequestMsg("test.reply", std::string(64, 'x')));
		CHECK(toWeb.size() == 1);
		// A missing or non-string id never dispatches either.
		bridge.HandleWebMessage("someview", R"({"kind":"request","name":"test.reply","payload":{}})");
		bridge.HandleWebMessage("someview", R"({"kind":"request","name":"test.reply","id":7,"payload":{}})");
		CHECK(toWeb.size() == 1);
		CHECK(LastProtocolFaultCode() == "invalid-request");
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
	g_protocolFaults.clear();
	toWeb.clear();
	bridge.HandleWebMessage("someview", SendMsg("acme.mymod.getWeight"));
	CHECK(toWeb.empty());
	CHECK(LastProtocolFaultCode() == "wrong-endpoint-kind");  // one name, one kind

	toWeb.clear(); g_request.reset();
	bridge.HandleWebMessage("request-view", RequestMsg("acme.mymod.getWeight", "rq1", R"({"unit":"kg"})"));
	CHECK(g_request.has_value()); CHECK(toWeb.empty());
	if (g_request) {
		CHECK(g_requestCommand == "acme.mymod.getWeight");
		CHECK(g_requestSource == "request-view");
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
	CHECK(!toWeb.empty() && toWeb.back().second.find("acme.mymod.weight") == std::string::npos);
	CHECK(LoggedContaining("WARN", "ignored second response"));

	toWeb.clear(); g_request.reset();
	bridge.HandleWebMessage("request-view", RequestMsg("acme.mymod.getWeight", "rq2"));
	bridge.Tick(std::chrono::steady_clock::now() + std::chrono::seconds(31));
	CHECK(toWeb.size() == 1);
	CHECK(Last(toWeb).value("kind", "") == "error");
	CHECK(Last(toWeb).value("id", "") == "rq2");
	CHECK(PayloadOf(Last(toWeb)).value("code", "") == "no-response");
	if (g_request) g_request->Respond(R"({"weight":99})");
	api.PumpMainThread();
	CHECK(toWeb.size() == 1);
	CHECK(LoggedContaining("WARN", "ignored late response for stale token"));

	toWeb.clear(); g_request.reset();
	bridge.HandleWebMessage("closing-view", RequestMsg("acme.mymod.getWeight", "rq3"));
	api.SetViewInstantiated("closing-view", false);
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

	toWeb.clear(); g_requests.clear();
	for (int i = 0; i < 65; ++i) {
		bridge.HandleWebMessage("cap-view", RequestMsg("acme.mymod.getWeight", std::format("cap{}", i)));
	}
	CHECK(g_requests.size() == 64);
	CHECK(toWeb.size() == 1);
	CHECK(Last(toWeb).value("id", "") == "cap64");
	CHECK(PayloadOf(Last(toWeb)).value("code", "") == "request-capacity");
	CHECK(LoggedContaining("WARN", "requests in flight"));
	api.SetViewInstantiated("cap-view", false);
	bridge.OnViewDestroyed("cap-view");

	{
		api.TakeViewStateOps();  // start from an empty queue

		// Validation is synchronous — the caller sees the false at the call.
		CHECK(!api.SetViewState(nullptr, "roster", "{}"));
		CHECK(!api.SetViewState("acme.mymod", nullptr, "{}"));
		CHECK(!api.SetViewState("acme.mymod", "roster", nullptr));
		CHECK(!api.SetViewState("acme.mymod", "", "{}"));
		CHECK(!api.SetViewState("../evil", "roster", "{}"));
		CHECK(!api.SetViewState("osfui", "roster", "{}"));
		CHECK(LoggedContaining("WARN", "refused SetViewState('osfui')"));
		CHECK(!api.SetViewState("acme.mymod", std::string(129, 'k').c_str(), "{}"));
		CHECK(LoggedContaining("WARN", "key longer than 128 characters"));
		CHECK(!api.SetViewState("acme.mymod", "roster", "{bad"));
		CHECK(LoggedContaining("WARN", "payload is not valid JSON"));
		CHECK(api.TakeViewStateOps().empty());  // nothing invalid was queued

		CHECK(api.SetViewState("acme.mymod", "roster", R"({"crew":2})"));
		CHECK(api.SetViewState("Mixed Mod_name!", "count", "7"));
		CHECK(api.SetViewState("acme.mymod", "roster", "[1,2]"));
		{
			const auto ops = api.TakeViewStateOps();
			CHECK(ops.size() == 3);
			CHECK(ops.size() == 3 && ops[0].mod == "acme.mymod" && ops[0].key == "roster");
			CHECK(ops.size() == 3 && ops[0].value.value("crew", 0) == 2);
			CHECK(ops.size() == 3 && ops[1].mod == "Mixed Mod_name!" && ops[1].key == "count" && ops[1].value == 7);
			CHECK(ops.size() == 3 && ops[2].key == "roster" && ops[2].value.is_array());
			CHECK(api.TakeViewStateOps().empty());  // drained
		}

		CHECK(api.SetViewState("acme.mymod", "kept", "{}"));
		api.PumpMainThread();
		CHECK(api.TakeViewStateOps().size() == 1);

		for (int i = 0; i < 300; ++i) {
			api.SetViewState("acme.mymod", ("k" + std::to_string(i)).c_str(), "{}");
		}
		const auto capped = api.TakeViewStateOps();
		CHECK(capped.size() == 256);
		CHECK(!capped.empty() && capped.front().key == "k0");  // oldest kept
		CHECK(LoggedContaining("WARN", "pending SetViewState queue full"));
	}

	// --- unsupported ABI-major callers, made visible ---------------------------
	{
		api.TakeUnsupportedApiCallers();
		api.NoteUnsupportedApiCaller("FutureMod.dll", 3, 5);
		api.NoteUnsupportedApiCaller("FutureMod.dll", 4, 0);
		api.NoteUnsupportedApiCaller("", 7, 2);
		{
			const auto callers = api.TakeUnsupportedApiCallers();
			CHECK(callers.size() == 2);
			CHECK(callers.size() == 2 && callers[0].module == "FutureMod.dll");
			CHECK(callers.size() == 2 && callers[0].major == 3 && callers[0].minor == 5);
			CHECK(callers.size() == 2 && callers[1].module.empty() && callers[1].major == 7);
		}
		CHECK(api.TakeUnsupportedApiCallers().empty());
		for (int i = 0; i < 40; ++i) {
			api.NoteUnsupportedApiCaller(std::format("mod{}.dll", i), 2, 0);
		}
		CHECK(api.TakeUnsupportedApiCallers().size() == 32);
	}

	// --- RegisterView takes qualified ids only (item 1) -----------------------
	CHECK(!api.RegisterView("osf"));              // unqualified: refused synchronously
	CHECK(!api.RegisterView("osfui.settings"));   // dotted join, not slash
	CHECK(api.RegisterView("Acme.Mod/dash"));     // opaque mixed-case mod id
	CHECK(api.RegisterView("acme.mymod/dash"));   // queued
	CHECK(!api.RegisterView("osfui/settings"));   // platform namespace is reserved
	{
		const auto regs = api.TakeViewRegistrations();
		CHECK(regs.size() == 2);
		CHECK(regs.size() == 2 && regs[0] == "Acme.Mod/dash" && regs[1] == "acme.mymod/dash");
	}

	// --- RequestMenu validates against discovery at queue time ----------------
	api.SetViewCatalog({ "acme.mymod/dash", "osfui/settings" });
	CHECK(!api.RequestMenu("acme.mymod/missing", true));  // typo: synchronous fallback signal

	api.SetBridgeAvailability(nullptr);
	api.PumpMainThread();
	CHECK(!api.IsBridgeReady());
	CHECK(api.SendToWeb("acme.mymod/dash", "acme.mymod.data", R"({"lazy":true})"));
	CHECK(api.RequestMenu("acme.mymod/dash", true));      // discovered, not instantiated: accepted
	CHECK(!api.RequestMenu("acme.mymod/dash", false));    // close must not instantiate
	{
		const auto requests = api.TakeViewPresentationRequests();
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
	api.SetBridgeAvailability(&lazyBridge);
	api.PumpMainThread();
	CHECK(api.IsBridgeReady());
	CHECK(toWeb.empty());  // an available bridge alone does not instantiate or greet this target document

	lazyBridge.OnViewCreated("acme.mymod/dash");
	api.SetViewInstantiated("acme.mymod/dash", true);
	api.PumpMainThread();
	CHECK(toWeb.empty());  // an instantiated view is not a greeted document
	lazyBridge.HandleWebMessage("acme.mymod/dash", kHello);
	CHECK(toWeb.size() == 2);
	CHECK(Envelope(toWeb, 0).value("kind", "") == "ready");
	CHECK(Envelope(toWeb, 1).value("kind", "") == "event");
	CHECK(Envelope(toWeb, 1).value("name", "") == "acme.mymod.data");

	// Once greeted, a send flushes to the wire on the next pump.
	toWeb.clear();
	CHECK(api.SendToWeb("acme.mymod/dash", "acme.mymod.data", R"({"lazy":true})"));
	api.PumpMainThread();
	CHECK(toWeb.size() == 1);
	CHECK(!toWeb.empty() && toWeb[0].first == "acme.mymod/dash");
	CHECK(Last(toWeb).value("kind", "") == "event");
	CHECK(PayloadOf(Last(toWeb)).value("lazy", false));

	// Runtime-facing Bridge queues share one snapshot lock and an empty atomic fast path.
	CHECK(api.SetViewState("batch.mod", "value", "7"));
	CHECK(api.RegisterView("batch.mod/panel"));
	CHECK(api.RequestMenu("acme.mymod/dash", true));
	CHECK(!api.RegisterSettingsSchema(nullptr));
	CHECK(!api.RegisterSettingsSchema("{bad"));
	CHECK(!api.RegisterSettingsSchema(R"([])"));
	CHECK(!api.RegisterSettingsSchema(R"({"title":"missing id"})"));
	CHECK(api.RegisterSettingsSchema(R"({"id":"batch.mod","groups":[{"settings":[{"key":"enabled","type":"bool","default":true}]}]})"));
	api.UnregisterSettingsSchema("batch.mod");
	CHECK(LoggedContaining("WARN", "[deprecated] RegisterSettingsSchema('batch.mod')"));
	{
		auto batch = api.TakePendingBatch();
		CHECK(batch.state.size() == 1 && batch.state[0].key == "value" && batch.state[0].value == 7);
		CHECK(batch.viewRegistrations == std::vector<std::string>{ "batch.mod/panel" });
		CHECK(batch.presentation.size() == 1 && batch.presentation[0].view == "acme.mymod/dash" && batch.presentation[0].open);
		CHECK(batch.schemas.size() == 2);
		if (batch.schemas.size() == 2) {
			CHECK(batch.schemas[0].schema.value("id", "") == "batch.mod");
			CHECK(batch.schemas[1].schema.is_null() && batch.schemas[1].modId == "batch.mod");
		}
		const auto empty = api.TakePendingBatch();
		CHECK(empty.state.empty() && empty.schemas.empty() && empty.viewRegistrations.empty() && empty.presentation.empty());
	}

	// The send registry re-applies to a replacement bridge, handlers intact.
	lazyBridge.HandleWebMessage("acme.mymod/dash", SendMsg("acme.mymod.catalog.get"));
	CHECK(g_firedA.size() == 3);
	api.SetBridgeAvailability(nullptr);
	api.PumpMainThread();

	CHECK(api.RequestMenu("acme.mymod/dash", false));
	{
		const auto requests = api.TakeViewPresentationRequests();
		CHECK(requests.size() == 1);
		if (requests.size() == 1) {
			CHECK(requests[0].view == "acme.mymod/dash");
			CHECK(!requests[0].open);
		}
	}

	api.SetViewInstantiated("acme.mymod/dash", false);
	api.SetBridgeAvailability(&lazyBridge);
	api.PumpMainThread();
	toWeb.clear();
	for (int i = 0; i < 65; ++i) {
		CHECK(api.SendToWeb("acme.mymod/dash", "acme.mymod.state",
			std::format(R"({{"seq":{}}})", i).c_str()));
	}
	api.PumpMainThread();
	CHECK(toWeb.empty());
	CHECK(LoggedContaining("WARN", "SendToWeb holdback"));
	api.SetViewInstantiated("acme.mymod/dash", true);
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
	api.SetBridgeAvailability(nullptr);
	api.PumpMainThread();

	api.SetViewCatalog({ "acme.mymod/dash", "osfui/settings", "acme.mymod/panel" });
	api.SetViewInstantiated("acme.mymod/panel", true);
	api.SetBridgeAvailability(&lazyBridge);
	lazyBridge.OnViewCreated("acme.mymod/panel");
	lazyBridge.HandleWebMessage("acme.mymod/panel", kHello);
	toWeb.clear();
	CHECK(api.SendToWeb("acme.mymod/panel", "acme.mymod.state", R"({"world":1})"));
	api.PumpMainThread();
	CHECK(toWeb.size() == 1 && toWeb[0].first == "acme.mymod/panel");
	CHECK(Last(toWeb).value("kind", "") == "event");
	api.SetBridgeAvailability(nullptr);
	api.PumpMainThread();

	// --- standalone service clients -------------------------------------------
	{
		using namespace OSFUI::API;

		TestServices runtime;
		Settings::Client settingsClient;
		Views::Client views;
		Diagnostics::Client diagnostics;
		CHECK(settingsClient.Attach(&runtime));
		CHECK(views.Attach(&runtime));
		CHECK(diagnostics.Attach(&runtime));
		CHECK(settingsClient.Version() == Settings::kVersion);
		CHECK(views.Version() == Views::kVersion);
		CHECK(diagnostics.Version() == Diagnostics::kVersion);
		CHECK(!views.Has(0x00010001u));

		views.RegisterSend("acme.mymod.send", &HandlerA, nullptr);
		views.UnregisterSend("acme.mymod.send");
		views.RegisterRequest("acme.mymod.request", &RequestHandler, nullptr);
		views.UnregisterRequest("acme.mymod.request");
		CHECK(runtime.sendCalls == 2);
		CHECK(runtime.requestCalls == 2);

		CHECK(views.RegisterRelativePointer("acme.mymod/panel", &RelativePointer, nullptr));
		views.UnregisterRelativePointer("acme.mymod/panel");
		CHECK(runtime.relativePointerCalls == 2);
		CHECK(views.RegisterViewOpenPreflight("acme.mymod/panel", &ViewOpenPreflight, &allow));
		views.UnregisterViewOpenPreflight("acme.mymod/panel");
		CHECK(runtime.viewOpenPreflightCalls == 2);
		CHECK(views.RegisterViewLifecycle("acme.mymod/panel", &ViewLifecycle, nullptr));
		views.UnregisterViewLifecycle("acme.mymod/panel");
		CHECK(runtime.viewLifecycleCalls == 2);

		Views::Client missingViews;
		CHECK(!missingViews);
		CHECK(!missingViews.RequestMenu("acme.mymod/panel", true));

		Settings::Client missingSettings;
		CHECK(!missingSettings);
		CHECK(missingSettings.GetSettingString("a.b", "k", nullptr, 0) == 0);

		Views::Client liveViews;
		CHECK(liveViews.Attach(static_cast<Views::IViews*>(&api)));
		CHECK(liveViews.SetViewState("acme.mymod", "wrapper", R"({"via":"client"})"));
		{
			const auto ops = api.TakeViewStateOps();
			CHECK(ops.size() == 1 && ops[0].key == "wrapper");
		}
		CHECK(liveViews.RegisterView("acme.mymod/extra"));
		CHECK(!liveViews.RegisterView("unqualified"));
		{
			const auto regs = api.TakeViewRegistrations();
			CHECK(regs.size() == 1 && regs[0] == "acme.mymod/extra");
		}
	}

	{
		using Op = OSFUI::API::BridgeApi::HealthIssueOp;
		using Sev = OSFUI::API::Diagnostics::IssueSeverity;
		const auto kWarn = static_cast<std::uint32_t>(Sev::kWarning);
		const auto kErr = static_cast<std::uint32_t>(Sev::kError);

		api.TakeHealthIssueOps();
		CHECK(!api.ReportIssue(nullptr, "x", "y", kWarn, "", nullptr));
		CHECK(!api.ReportIssue("", "x", "y", kWarn, "", nullptr));
		CHECK(!api.ReportIssue("osfui", "x", "y", kWarn, "", nullptr));
		CHECK(!api.ReportIssue("bad/name", "x", "y", kWarn, "", nullptr));
		CHECK(LoggedContaining("WARN", "refused ReportIssue('osfui')"));
		CHECK(!api.ReportIssue("acme.mymod", "", "y", kWarn, "", nullptr));
		CHECK(!api.ReportIssue("acme.mymod", "x", "", kWarn, "", nullptr));
		CHECK(!api.ReportIssue("acme.mymod", "x", "y", kWarn, "", "[1,2]"));
		CHECK(!api.ReportIssue("acme.mymod", "x", "y", kWarn, "", "\"nope\""));
		CHECK(!api.ReportIssue("acme.mymod", "x", "y", kWarn, "", "{not json"));
		CHECK(api.TakeHealthIssueOps().empty());

		CHECK(api.ReportIssue("acme.mymod", "pack-parse", "catalog.parse-failed", kErr,
			"highlights", "{\"file\":\"C:\\\\Mods\\\\packs\\\\bad.json\",\"line\":12}"));
		CHECK(api.ReportIssue("acme.mymod", "quiet", "audio.missing", kWarn, "", nullptr));
		CHECK(api.ReportIssue("acme.mymod", "future", "odd.tier", 9u, "", ""));
		CHECK(LoggedContaining("WARN", "unknown severity 9"));
		CHECK(api.ClearIssue("acme.mymod", "quiet"));
		CHECK(!api.ClearIssue("acme.mymod", ""));
		CHECK(!api.ClearIssue("bogus/id", "quiet"));
		CHECK(api.ClearIssuesExcept("acme.mymod", "[\"pack-parse\"]"));
		CHECK(api.ClearIssuesExcept("acme.mymod", "[]"));
		CHECK(api.ClearIssuesExcept("acme.mymod", nullptr));
		CHECK(!api.ClearIssuesExcept("acme.mymod", "{\"a\":1}"));
		CHECK(!api.ClearIssuesExcept("acme.mymod", "[\"ok\", 7]"));

		const auto ops = api.TakeHealthIssueOps();
		CHECK(ops.size() == 7);
		CHECK(ops[0].kind == Op::Kind::kReport && ops[0].modId == "acme.mymod");
		CHECK(ops[0].id == "pack-parse" && ops[0].code == "catalog.parse-failed");
		CHECK(ops[0].error && ops[0].subject == "highlights");
		CHECK(ops[0].context.value("line", 0) == 12);
		CHECK(!ops[1].error);
		CHECK(ops[2].error);
		CHECK(ops[3].kind == Op::Kind::kClear && ops[3].id == "quiet");
		CHECK(ops[4].kind == Op::Kind::kClearExcept && ops[4].keep.size() == 1 &&
		      ops[4].keep[0] == "pack-parse");
		CHECK(ops[5].kind == Op::Kind::kClearExcept && ops[5].keep.empty());
		CHECK(ops[6].kind == Op::Kind::kClearExcept && ops[6].keep.empty());
		CHECK(api.TakeHealthIssueOps().empty());

		for (int i = 0; i < 300; ++i) {
			api.ReportIssue("acme.mymod", ("id" + std::to_string(i)).c_str(), "spam", kWarn, "", nullptr);
		}
		const auto capped = api.TakeHealthIssueOps();
		CHECK(capped.size() == 256);
		CHECK(capped.front().id == "id0");
		CHECK(LoggedContaining("WARN", "health reports already queued"));
	}

	// --- teardown: bridge going away must not dangle --------------------------
	api.SetBridgeAvailability(nullptr);
	api.PumpMainThread();
	CHECK(!api.IsBridgeReady());

	std::fprintf(stderr, "bridge_api_tests: %d checks, %d failure(s)\n", g_checks, g_failures);
	return g_failures;
}
