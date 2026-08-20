#include "API/BridgeApi.h"
#include "API/PapyrusApi.h"

#include "RE/B/BSScriptUtil.h"
#include "RE/E/Events.h"
#include "check.h"

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
	using IVM = RE::BSScript::IVirtualMachine;
	using Str = RE::BSFixedString;
	using Var = RE::BSScript::Variable;

	auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
	API::Papyrus::Install();

	// The public VM surface is split by responsibility; no unpublished legacy
	// native remains bound on OSFUI.
	CHECK((vm->nativeScripts["IsAvailable"] == std::vector<std::string>{ "OSFUI" }));
	CHECK((vm->nativeScripts["GetBool"] == std::vector<std::string>{ "OSFUI_Settings" }));
	CHECK((vm->nativeScripts["SetState"] == std::vector<std::string>{ "OSFUI_View" }));
	CHECK((vm->nativeScripts["Unregister"] ==
		std::vector<std::string>{ "OSFUI_Settings", "OSFUI_View" }));
	for (const auto* old : { "SetViewInt", "SendViewEvent", "ListenForViewActions",
		"PushToView", "RegisterForViewActions", "GetFormById" }) {
		CHECK(!vm->natives.contains(old));
	}

	const auto isAvailable =
		vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate)>("IsAvailable");
	const auto getVersion =
		vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate)>("GetVersion");
	const auto getVersionString =
		vm->GetNative<Str (*)(IVM&, std::uint32_t, std::monostate)>("GetVersionString");
	CHECK(isAvailable(*vm, 0, {}));
	CHECK(getVersion(*vm, 0, {}) > 0);
	CHECK(std::string_view(getVersionString(*vm, 0, {}).c_str()).size() > 0);

	// Typed settings reads use the any-thread mirror; writes report queue
	// admission and are applied later on the main thread.
	auto& mirror = API::BridgeApi::Get().Mirror();
	mirror.Update("Acme.Mod", "Enabled", true);
	mirror.Update("Acme.Mod", "Count", 42);
	mirror.Update("Acme.Mod", "Scale", 1.5);
	mirror.Update("Acme.Mod", "Label", "hello");
	const auto getBool = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate,
		Str, Str, bool)>("GetBool");
	const auto getInt = vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate,
		Str, Str, std::int32_t)>("GetInt");
	const auto getFloat = vm->GetNative<float (*)(IVM&, std::uint32_t, std::monostate,
		Str, Str, float)>("GetFloat");
	const auto getString = vm->GetNative<Str (*)(IVM&, std::uint32_t, std::monostate,
		Str, Str, Str)>("GetString");
	CHECK(getBool(*vm, 0, {}, "acme.mod", "enabled", false));
	CHECK(getInt(*vm, 0, {}, "acme.mod", "count", 0) == 42);
	CHECK(getFloat(*vm, 0, {}, "acme.mod", "scale", 0.0f) == 1.5f);
	CHECK(std::string(getString(*vm, 0, {}, "acme.mod", "label", "fallback").c_str()) == "hello");
	CHECK(getInt(*vm, 0, {}, "acme.mod", "missing", 9) == 9);

	const auto setBool = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate,
		Str, Str, bool)>("SetBool");
	const auto setInt = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate,
		Str, Str, std::int32_t)>("SetInt");
	const auto reset = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate,
		Str, Str)>("Reset");
	CHECK(setBool(*vm, 0, {}, "Acme.Mod", "Enabled", false));
	CHECK(setInt(*vm, 0, {}, "Acme.Mod", "Count", 7));
	CHECK(reset(*vm, 0, {}, "Acme.Mod", ""));
	CHECK(!setBool(*vm, 0, {}, "../evil", "Enabled", true));
	CHECK(!setBool(*vm, 0, {}, "Acme.Mod", std::string(129, 'k'), true));
	{
		auto batch = API::Papyrus::TakePendingBatch();
		CHECK(batch.settings.size() == 3);
		CHECK(batch.settings.size() == 3 && batch.settings[0].mod == "acme.mod");
		CHECK(batch.settings.size() == 3 && batch.settings[0].value == false);
		CHECK(batch.settings.size() == 3 && batch.settings[2].reset && batch.settings[2].key.empty());
	}

	// Fixed listener callbacks keep registration shape out of the public API.
	const auto listenChanges = vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t,
		std::monostate, Str, Str, Str)>("ListenForChangesStatic");
	const auto listenHotkeys = vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t,
		std::monostate, Str, Str, Str)>("ListenForHotkeysStatic");
	const auto unregister = vm->GetNative<bool (*)(IVM&, std::uint32_t,
		std::monostate, std::int32_t)>("Unregister");
	const auto settingToken = listenChanges(*vm, 0, {}, "SettingsSink", "Acme.Mod", "");
	const auto hotkeyToken = listenHotkeys(*vm, 0, {}, "HotkeySink", "Acme.Mod", "Jump");
	CHECK(settingToken != 0 && hotkeyToken != 0);
	vm->calls.clear();
	API::Papyrus::OnSettingChanged("acme.mod", "Count");
	API::Papyrus::OnHotkey("acme.mod", "Other");
	API::Papyrus::OnHotkey("ACME.MOD", "jump");
	CHECK(vm->calls.size() == 2);
	if (vm->calls.size() == 2) {
		CHECK(vm->calls[0].fn == "OnOSFUISettingChanged");
		CHECK((vm->calls[0].args == std::vector<std::string>{ "acme.mod", "Count" }));
		CHECK(vm->calls[1].fn == "OnOSFUIHotkey");
		CHECK((vm->calls[1].args == std::vector<std::string>{ "ACME.MOD", "jump" }));
	}

	// Endpoint ownership is exact and first-wins across send and request kinds.
	const auto registerSend = vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t,
		std::monostate, Str, Str, Str)>("RegisterSendStatic");
	const auto registerRequest = vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t,
		std::monostate, Str, Str, Str)>("RegisterRequestStatic");
	const auto sendToken = registerSend(*vm, 0, {}, "ViewSink", "Acme.Mod", "equip");
	CHECK(sendToken != 0);
	CHECK(registerSend(*vm, 0, {}, "ViewSink", "Acme.Mod", "close") == 0);
	CHECK(registerSend(*vm, 0, {}, "ViewSink", "Acme.Mod", "papyrus.call") == 0);
	CHECK(registerSend(*vm, 0, {}, "ViewSink", "Acme.Mod", "osfui.private") == 0);
	CHECK(registerSend(*vm, 0, {}, "ViewSink", "Acme.Mod", std::string(120, 'x')) == 0);
	const auto boundaryToken = registerSend(*vm, 0, {}, "ViewSink", "Acme.Mod", std::string(119, 'x'));
	CHECK(boundaryToken != 0);  // 8-byte mod + dot + 119-byte local = 128
	CHECK(unregister(*vm, 0, {}, boundaryToken));
	CHECK(registerRequest(*vm, 0, {}, "OtherSink", "acme.mod", "EQUIP") == 0);
	const auto requestToken = registerRequest(*vm, 0, {}, "ViewSink", "Acme.Mod", "lookup");
	CHECK(requestToken != 0);
	CHECK(registerSend(*vm, 0, {}, "OtherSink", "acme.mod", "LOOKUP") == 0);

	const auto localSend = API::Papyrus::ResolveViewEndpoint("acme.mod", "equip");
	CHECK(localSend.kind == API::Papyrus::ViewEndpointKind::kSend);
	CHECK(localSend.modId == "acme.mod" && localSend.name == "equip");
	const auto qualifiedRequest = API::Papyrus::ResolveViewEndpoint("osfui", "ACME.MOD.lookup");
	CHECK(qualifiedRequest.kind == API::Papyrus::ViewEndpointKind::kRequest);
	CHECK(qualifiedRequest.modId == "acme.mod" && qualifiedRequest.name == "lookup");
	CHECK(API::Papyrus::ResolveViewEndpoint("other.mod", "equip").kind ==
		API::Papyrus::ViewEndpointKind::kNone);  // unqualified spoof cannot cross mods
	CHECK(API::Papyrus::ResolveViewEndpoint("other.mod", "acme.mod.missing").kind ==
		API::Papyrus::ViewEndpointKind::kNone);

	vm->calls.clear();
	CHECK(API::Papyrus::OnViewSend("acme.mod", "equip",
		{ true, std::int32_t{ 7 }, 1.25f, std::string{ "laser" }, std::monostate{} },
		"acme.mod/inventory"));
	CHECK(vm->calls.size() == 1);
	if (vm->calls.size() == 1) {
		const auto& call = vm->calls[0];
		CHECK(call.isStatic && call.scriptName == "ViewSink" && call.fn == "OnOSFUISend");
		CHECK((call.args == std::vector<std::string>{
			"equip", "true", "7", "1.250000", "laser", "", "acme.mod/inventory" }));
		CHECK((call.argTypes == std::vector<std::string>{
			"string", "bool", "int", "float", "string", "none", "string" }));
	}
	CHECK(!API::Papyrus::OnViewSend("other.mod", "equip", {}, "other.mod/view"));

	// Request callbacks receive the authoritative caller before the opaque reply
	// token. Reply resolves to a raw JSON value, not a { value } wrapper.
	vm->calls.clear();
	CHECK(API::Papyrus::OnViewRequest("acme.mod", "lookup", { std::string{ "key" } },
		"other.mod/panel", "defer-1"));
	CHECK(vm->calls.size() == 1);
	std::string replyToken;
	if (vm->calls.size() == 1) {
		const auto& call = vm->calls[0];
		CHECK(call.fn == "OnOSFUIRequest");
		CHECK(call.args.size() == 4);
		if (call.args.size() == 4) {
			CHECK(call.args[0] == "lookup" && call.args[1] == "key");
			CHECK(call.args[2] == "other.mod/panel");
			replyToken = call.args[3];
		}
	}
	const auto reply = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate,
		Str, const Var*)>("Reply");
	Var answer;
	answer = std::int32_t{ 42 };
	CHECK(!replyToken.empty() && reply(*vm, 0, {}, replyToken, &answer));
	CHECK(!reply(*vm, 0, {}, replyToken, &answer));
	{
		auto batch = API::Papyrus::TakePendingBatch();
		CHECK(batch.replies.size() == 1);
		CHECK(batch.replies.size() == 1 && batch.replies[0].view == "other.mod/panel");
		CHECK(batch.replies.size() == 1 && batch.replies[0].deferToken == "defer-1");
		CHECK(batch.replies.size() == 1 && !batch.replies[0].rejected && batch.replies[0].value == 42);
	}

	const auto reject = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate,
		Str, Str, Str)>("Reject");
	vm->calls.clear();
	CHECK(API::Papyrus::OnViewRequest("acme.mod", "lookup", {}, "osfui/settings", "defer-2"));
	CHECK(vm->calls.size() == 1);
	if (vm->calls.size() == 1) replyToken = vm->calls[0].args.back();
	CHECK(reject(*vm, 0, {}, replyToken, "not-found", "missing"));
	{
		auto batch = API::Papyrus::TakePendingBatch();
		CHECK(batch.replies.size() == 1 && batch.replies[0].rejected);
		CHECK(batch.replies.size() == 1 && batch.replies[0].code == "not-found");
	}

	// State is retained, events are one-shot, and both carry actual JSON values.
	const auto setState = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate,
		Str, Str, const Var*)>("SetState");
	const auto setStateInts = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate,
		Str, Str, std::vector<std::int32_t>)>("SetStateInts");
	const auto emitEvent = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate,
		Str, Str, std::optional<std::vector<const Var*>>)>("EmitEvent");
	Var enabled;
	enabled = true;
	Var label;
	label = Str{ "ready" };
	CHECK(setState(*vm, 0, {}, "Acme.Mod", "enabled", &enabled));
	CHECK(setStateInts(*vm, 0, {}, "Acme.Mod", "counts", { 2, 3 }));
	CHECK(!setState(*vm, 0, {}, "Acme.Mod", "", &enabled));
	CHECK(!setState(*vm, 0, {}, "Acme.Mod", std::string(129, 'k'), &enabled));
	CHECK(emitEvent(*vm, 0, {}, "Acme.Mod", "changed",
		std::vector<const Var*>{ &label, &enabled }));
	CHECK(emitEvent(*vm, 0, {}, "Acme.Mod", "empty", std::nullopt));
	{
		auto batch = API::Papyrus::TakePendingBatch();
		CHECK(batch.states.size() == 2);
		CHECK(batch.states.size() == 2 && batch.states[0].key == "enabled" && batch.states[0].value == true);
		CHECK(batch.states.size() == 2 && batch.states[1].value == nlohmann::json::array({ 2, 3 }));
		CHECK(batch.events.size() == 2);
		CHECK(batch.events.size() == 2 && batch.events[0].args == nlohmann::json::array({ "ready", true }));
		CHECK(batch.events.size() == 2 && batch.events[1].args.empty());
	}

	API::BridgeApi::Get().SetViewCatalog({ "acme.mod/panel" });
	const auto open = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate, Str)>("Open");
	const auto close = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate, Str)>("Close");
	CHECK(open(*vm, 0, {}, "Acme.Mod/Panel"));
	CHECK(!close(*vm, 0, {}, "Acme.Mod/Panel"));
	API::BridgeApi::Get().SetViewInstantiated("acme.mod/panel", true);
	CHECK(close(*vm, 0, {}, "Acme.Mod/Panel"));
	CHECK(API::BridgeApi::Get().TakeViewPresentationRequests().size() == 2);

	CHECK(unregister(*vm, 0, {}, sendToken));
	CHECK(!unregister(*vm, 0, {}, sendToken));
	CHECK(API::Papyrus::ResolveViewEndpoint("acme.mod", "equip").kind ==
		API::Papyrus::ViewEndpointKind::kNone);

	// Registrations and queued view work are session-scoped.
	RE::TESLoadGameEvent::GetEventSource()->Notify(RE::TESLoadGameEvent{});
	CHECK(API::Papyrus::ResolveViewEndpoint("acme.mod", "lookup").kind ==
		API::Papyrus::ViewEndpointKind::kNone);
	const auto resetBatch = API::Papyrus::TakePendingBatch();
	CHECK(resetBatch.sessionReset);
	CHECK(!API::Papyrus::TakePendingBatch().sessionReset);

	std::fprintf(stderr, "papyrus_action_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures;
}
