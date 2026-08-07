#include "compat/v1/NativeBridge.h"

#include "api/BridgeApi.h"
#include "runtime/MessageBridge.h"

namespace
{
	int failures = 0;
#define CHECK(expr) do { if (!(expr)) { ++failures; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } } while (0)

	struct Fire
	{
		std::string name;
		nlohmann::json payload;
		std::string source;
	};
	std::vector<Fire> fires;
	void Command(const char* a_name, const char* a_json, const char* a_source, void*) noexcept
	{
		fires.push_back({ a_name, nlohmann::json::parse(a_json), a_source });
	}

	std::optional<OSFUI::Compat::V1::Request> request;
	std::string requestCommand;
	nlohmann::json requestPayload;
	std::string requestSource;
	void Request(const OSFUI::Compat::V1::Request& a_request, void*) noexcept
	{
		requestCommand = a_request.command ? a_request.command : "";
		requestPayload = nlohmann::json::parse(a_request.payloadJson ? a_request.payloadJson : "{}");
		requestSource = a_request.sourceViewId ? a_request.sourceViewId : "";
		// The pointer fields are callback-scoped in the frozen ABI; the copied
		// token/thunks remain valid for a later asynchronous response.
		request = a_request;
	}

	struct HotkeyFire { std::string mod; std::string key; };
	std::vector<HotkeyFire> hotkeys;
	void Hotkey(const char* a_mod, const char* a_key, void*) noexcept
	{
		hotkeys.push_back({ a_mod, a_key });
	}

	std::size_t readyCalls = 0;
	void Ready(void*) noexcept { ++readyCalls; }

	struct SettingFire { std::string mod; std::string key; nlohmann::json value; };
	std::vector<SettingFire> settings;
	void Setting(const char* a_mod, const char* a_key, const char* a_value, void*) noexcept
	{
		settings.push_back({ a_mod, a_key, nlohmann::json::parse(a_value) });
	}
}

namespace OSFUI::Log
{
	void WarnOnce(std::once_flag& a_flag, std::string_view a_message)
	{
		std::call_once(a_flag, [&] { REX::test::Log("WARN", std::string(a_message)); });
	}
	bool DevMode() { return true; }
	void SetDevMode(bool) {}
}

int main()
{
	using namespace OSFUI;
	using Compat::V1::SupportsRequestedAbi;
	for (std::uint32_t minor = 0; minor <= 8; ++minor) {
		CHECK(SupportsRequestedAbi((1u << 16) | minor));
	}
	CHECK(!SupportsRequestedAbi(0));
	CHECK(!SupportsRequestedAbi((2u << 16) | 0u));
	CHECK(!SupportsRequestedAbi((3u << 16) | 4u));

	auto& api = API::BridgeApi::Get();
	auto& legacy = Compat::V1::NativeBridge::Get();
	Compat::V1::IOSFUIBridge* bridgeVtable = &legacy;
	CHECK(bridgeVtable->GetInterfaceVersion() == ((1u << 16) | 8u));
	std::uint32_t major = 0, minor = 0, patch = 0;
	bridgeVtable->GetPluginVersion(major, minor, patch);
	CHECK(major == 2 && minor == 0 && patch == 0);
	CHECK(std::string_view(bridgeVtable->GetBridgeProtocolVersion()) == "2.0");

	std::vector<nlohmann::json> outbox;
	MessageBridge web([&](std::string_view, std::string_view json) {
		outbox.push_back(nlohmann::json::parse(json));
	});
	api.SetViewCatalog({ "acme.widgets/panel" });
	api.SetViewInstantiated("acme.widgets/panel", true);
	bridgeVtable->SetReadyCallback(&Ready, nullptr);
	api.SetBridgeAvailability(&web);
	web.OnViewCreated("acme.widgets/panel", true);
	web.HandleWebMessage("acme.widgets/panel", R"({"kind":"send","name":"osfui.hello","payload":{}})");
	api.PumpMainThread();
	CHECK(readyCalls == 1);
	CHECK(bridgeVtable->IsBridgeReady());
	CHECK(bridgeVtable->RequestMenu("acme.widgets/panel", true));
	CHECK(api.TakeViewPresentationRequests().size() == 1);
	CHECK(bridgeVtable->RegisterView("acme.widgets/panel"));
	CHECK(api.TakeViewRegistrations() == std::vector<std::string>{ "acme.widgets/panel" });
	CHECK(bridgeVtable->SendToWeb("acme.widgets/panel", "acme.widgets.notice", R"({"ok":true})"));
	api.PumpMainThread();
	CHECK(outbox.back()["kind"] == "event" && outbox.back()["name"] == "acme.widgets.notice");

	// In 1.x request() could correlate any ui.command. A strict send endpoint
	// therefore still executes and auto-acks for a legacy document, while the
	// same wrong-kind request from a 2.0 document remains rejected.
	std::size_t strictSendCalls = 0;
	web.RegisterSend("acme.widgets.strict-send", [&](const nlohmann::json& payload, MessageBridge&) {
		++strictSendCalls;
		CHECK(payload.value("value", 0) == 5);
	});
	web.HandleWebMessage("acme.widgets/panel",
		R"({"kind":"request","name":"acme.widgets.strict-send","id":"send-q1","payload":{"value":5}})");
	CHECK(strictSendCalls == 1);
	CHECK(outbox.back()["kind"] == "reply" && outbox.back()["id"] == "send-q1");
	CHECK(outbox.back()["payload"]["ok"] == true);
	web.OnViewCreated("acme.widgets/strict", false);
	web.HandleWebMessage("acme.widgets/strict",
		R"({"kind":"request","name":"acme.widgets.strict-send","id":"send-q2","payload":{"value":5}})");
	CHECK(strictSendCalls == 1);
	CHECK(outbox.back()["kind"] == "error" && outbox.back()["id"] == "send-q2");
	CHECK(outbox.back()["payload"]["code"] == "wrong-endpoint-kind");

	bridgeVtable->RegisterCommand("acme.widgets.legacy", &Command, nullptr);
	api.PumpMainThread();
	web.HandleWebMessage("acme.widgets/panel",
		R"({"kind":"send","name":"acme.widgets.legacy","payload":{"value":7}})");
	CHECK(fires.size() == 1);
	CHECK(fires.back().payload == (nlohmann::json{ { "value", 7 } }));
	CHECK(!fires.back().payload.contains("requestId"));

	web.HandleWebMessage("acme.widgets/panel",
		R"({"kind":"request","name":"acme.widgets.legacy","id":"page-q1","payload":{"value":8}})");
	CHECK(fires.size() == 2);
	CHECK(fires.back().payload["requestId"] == "page-q1");
	CHECK(!outbox.empty() && outbox.back()["kind"] == "reply");
	CHECK(outbox.back()["id"] == "page-q1");
	CHECK(outbox.back()["payload"] == nlohmann::json({ { "ok", true }, { "command", "acme.widgets.legacy" } }));

	bridgeVtable->RegisterRequest("acme.widgets.request", &Request, nullptr);
	api.PumpMainThread();
	web.HandleWebMessage("acme.widgets/panel",
		R"({"kind":"request","name":"acme.widgets.request","id":"page-q2","payload":{"item":42}})");
	CHECK(request.has_value());
	CHECK(requestCommand == "acme.widgets.request");
	CHECK(requestPayload["item"] == 42);
	CHECK(requestSource == "acme.widgets/panel");
	request->Respond("legacy.result", R"({"accepted":true})");
	api.PumpMainThread();
	CHECK(outbox.back()["kind"] == "reply" && outbox.back()["id"] == "page-q2");
	CHECK(outbox.back()["payload"]["__osfuiV1Reply"]["type"] == "legacy.result");
	CHECK(outbox.back()["payload"]["__osfuiV1Reply"]["payload"]["accepted"] == true);

	// Suit Protocol's 1.7 path: runtime schema registration, mirrored typed
	// setting reads, and two hotkey subscriptions through the frozen slots.
	CHECK(bridgeVtable->RegisterSettingsSchema(
		R"({"id":"acme.widgets","groups":[{"settings":[{"key":"enabled","type":"bool","default":true}]}]})"));
	CHECK(api.TakeSchemaOps().size() == 1);
	api.Mirror().Update("acme.widgets", "enabled", true);
	api.Mirror().Update("acme.widgets", "count", std::int64_t{ 7 });
	api.Mirror().Update("acme.widgets", "scale", 1.5);
	api.Mirror().Update("acme.widgets", "mode", "compact");
	bool enabled = false;
	CHECK(bridgeVtable->GetSettingBool("acme.widgets", "enabled", &enabled) && enabled);
	std::int64_t count = 0;
	double scale = 0.0;
	CHECK(bridgeVtable->GetSettingInt("acme.widgets", "count", &count) && count == 7);
	CHECK(bridgeVtable->GetSettingFloat("acme.widgets", "scale", &scale) && scale == 1.5);
	char mode[16]{};
	CHECK(bridgeVtable->GetSettingString("acme.widgets", "mode", mode, sizeof(mode)) == 8);
	CHECK(std::string_view(mode) == "compact");
	const auto settingToken = bridgeVtable->SubscribeSettings("acme.widgets", &Setting, nullptr);
	CHECK(settingToken != 0);
	api.PumpMainThread();
	CHECK(settings.size() == 4);
	bridgeVtable->UnsubscribeSettings(settingToken);
	const auto first = bridgeVtable->SubscribeHotkey("acme.widgets", "toggle", &Hotkey, nullptr);
	const auto second = bridgeVtable->SubscribeHotkey("acme.widgets", "action", &Hotkey, nullptr);
	CHECK(first != 0 && second != 0 && first != second);
	api.Hotkeys().OnFired("acme.widgets", "toggle");
	api.Hotkeys().OnFired("acme.widgets", "action");
	api.Hotkeys().Pump();
	CHECK(hotkeys.size() == 2);
	bridgeVtable->UnsubscribeHotkey(first);
	bridgeVtable->UnsubscribeHotkey(second);

	CHECK(bridgeVtable->ReportIssue("acme.widgets", "legacy", "catalog.old", 0,
		"panel", R"({"format":1})"));
	CHECK(bridgeVtable->ClearIssue("acme.widgets", "legacy"));
	CHECK(bridgeVtable->ClearIssuesExcept("acme.widgets", R"([])"));
	CHECK(api.TakeHealthIssueOps().size() == 3);

	CHECK(bridgeVtable->SetViewState("acme.widgets", "status", R"({"ready":true})"));
	CHECK(api.TakeViewStateOps().size() == 1);
	bridgeVtable->UnregisterSettingsSchema("acme.widgets");
	const auto schemaRemovals = api.TakeSchemaOps();
	CHECK(schemaRemovals.size() == 1 && schemaRemovals[0].modId == "acme.widgets" &&
		schemaRemovals[0].schema.is_null());
	bridgeVtable->UnregisterCommand("acme.widgets.legacy");
	bridgeVtable->UnregisterRequest("acme.widgets.request");
	api.PumpMainThread();

	std::printf("v1 native bridge tests: %s\n", failures ? "FAILED" : "passed");
	return failures;
}
