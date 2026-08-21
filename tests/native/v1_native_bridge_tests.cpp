#include "API/BridgeApi.h"
#include "OSFUI_API.h"
#include "check.h"
#include "Bridge/MessageBridge.h"

namespace
{
	struct FrozenAbi18Bridge
	{
		virtual std::uint32_t GetInterfaceVersion() = 0;
		virtual void GetPluginVersion(std::uint32_t&, std::uint32_t&, std::uint32_t&) = 0;
		virtual const char* GetBridgeProtocolVersion() = 0;
		virtual bool IsBridgeReady() = 0;
		virtual void RegisterCommand(const char*, OSFUI::API::CommandFn, void*) = 0;
		virtual void UnregisterCommand(const char*) = 0;
		virtual bool SendToWeb(const char*, const char*, const char*) = 0;
		virtual void SetReadyCallback(OSFUI::API::ReadyFn, void*) = 0;
		virtual bool RequestMenu(const char*, bool) = 0;
		virtual std::uint32_t SubscribeSettings(const char*, OSFUI::API::SettingChangedFn, void*) = 0;
		virtual void UnsubscribeSettings(std::uint32_t) = 0;
		virtual bool GetSettingBool(const char*, const char*, bool*) = 0;
		virtual bool GetSettingInt(const char*, const char*, std::int64_t*) = 0;
		virtual bool GetSettingFloat(const char*, const char*, double*) = 0;
		virtual std::uint32_t GetSettingString(const char*, const char*, char*, std::uint32_t) = 0;
		virtual bool RegisterSettingsSchema(const char*) = 0;
		virtual void UnregisterSettingsSchema(const char*) = 0;
		virtual std::uint32_t SubscribeHotkey(const char*, const char*, OSFUI::API::HotkeyFn, void*) = 0;
		virtual void UnsubscribeHotkey(std::uint32_t) = 0;
		virtual bool RegisterView(const char*) = 0;
		virtual bool ReportIssue(const char*, const char*, const char*, std::uint32_t,
			const char*, const char*) = 0;
		virtual bool ClearIssue(const char*, const char*) = 0;
		virtual bool ClearIssuesExcept(const char*, const char*) = 0;
		virtual void RegisterRequest(const char*, OSFUI::API::RequestFn, void*) = 0;
		virtual void UnregisterRequest(const char*) = 0;
		virtual bool SetViewState(const char*, const char*, const char*) = 0;
	};

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

	std::optional<OSFUI::API::Request> request;
	std::string requestCommand;
	nlohmann::json requestPayload;
	std::string requestSource;
	void Request(const OSFUI::API::Request& a_request, void*) noexcept
	{
		requestCommand = a_request.command ? a_request.command : "";
		requestPayload = nlohmann::json::parse(a_request.payloadJson ? a_request.payloadJson : "{}");
		requestSource = a_request.sourceViewId ? a_request.sourceViewId : "";
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
	bool DebugEnabled() { return true; }
	void SetDebugLogging(bool) {}
}

int main()
{
	using namespace OSFUI;
	auto& api = API::BridgeApi::Get();
	auto* bridgeVtable = reinterpret_cast<FrozenAbi18Bridge*>(&api);
	CHECK(bridgeVtable->GetInterfaceVersion() == ((1u << 16) | 11u));
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

	CHECK(bridgeVtable->RegisterSettingsSchema(
		R"({"id":"acme.widgets","groups":[{"settings":[{"key":"enabled","type":"bool","default":true}]}]})"));
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
	{
		const auto batch = api.TakePendingBatch();
		CHECK(batch.schemas.size() == 2);
		if (batch.schemas.size() == 2) {
			CHECK(batch.schemas[0].schema.value("id", "") == "acme.widgets");
			CHECK(batch.schemas[1].schema.is_null() && batch.schemas[1].modId == "acme.widgets");
		}
	}
	bridgeVtable->UnregisterCommand("acme.widgets.legacy");
	bridgeVtable->UnregisterRequest("acme.widgets.request");
	api.PumpMainThread();

	std::printf("native ABI compatibility tests: %s\n", g_failures ? "FAILED" : "passed");
	return g_failures;
}
