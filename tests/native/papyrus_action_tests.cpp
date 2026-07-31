// Host-side tests for the Papyrus dynamic-data surface (PushToView +
// RegisterForViewActions, docs/authoring-dynamic-data.md): the REAL
// api/PapyrusApi.cpp compiled against stubs/RE (a recording VM), driven
// through the same natives the game binds. Covers the action-dispatch
// registry (case-insensitive mod filter, static/instance targets, token
// release, kind isolation, load-game teardown) and the PushToView queue/
// drain (canonical folding, validation, the drop-newest cap).
// Assert-style; process exit code is the failure count.

#include "api/BridgeApi.h"
#include "api/PapyrusApi.h"

#include "RE/B/BSScriptUtil.h"
#include "RE/E/Events.h"

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

	// Count of test-log entries whose text contains a_needle.
	std::size_t LogCount(std::string_view a_needle)
	{
		std::size_t n = 0;
		for (const auto& e : REX::test::Entries()) {
			if (e.find(a_needle) != std::string::npos) {
				++n;
			}
		}
		return n;
	}
}

// core/Log.h declarations (real impl pulls game deps — stub, as in
// settings_module_tests.cpp; SettingsStore references these).
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
	using IVM = RE::BSScript::IVirtualMachine;
	using Str = RE::BSFixedString;
	using ObjPtr = RE::BSTSmartPointer<RE::BSScript::Object>;

	auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();

	// Install binds the natives on the stub VM and hooks the load-game source.
	API::Papyrus::Install();
	CHECK(vm->natives.contains("PushToView"));
	CHECK(vm->natives.contains("SetViewInt"));
	CHECK(vm->natives.contains("SetViewStrings"));
	CHECK(vm->natives.contains("ListenForViewActions"));
	CHECK(vm->natives.contains("ListenForViewRequests"));
	CHECK(vm->natives.contains("ReplyViewInt"));
	CHECK(vm->natives.contains("RegisterForViewActions"));
	CHECK(vm->natives.contains("RegisterForViewActionsStatic"));
	CHECK(vm->natives.contains("RegisterForViewActionsArgs"));
	CHECK(vm->natives.contains("RegisterForViewActionsArgsStatic"));
	CHECK(vm->natives.contains("Unregister"));

	const auto registerStatic =
		vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate, Str, Str, Str)>("RegisterForViewActionsStatic");
	const auto registerInstance =
		vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate, ObjPtr, Str, Str)>("RegisterForViewActions");
	const auto registerArgsStatic =
		vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate, Str, Str, Str)>("RegisterForViewActionsArgsStatic");
	const auto registerArgsInstance =
		vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate, ObjPtr, Str, Str)>("RegisterForViewActionsArgs");
	const auto registerSettingsStatic =
		vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate, Str, Str, Str)>("RegisterForSettingChangesStatic");
	const auto unregister =
		vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate, std::int32_t)>("Unregister");
	const auto pushToView =
		vm->GetNative<void (*)(IVM&, std::uint32_t, std::monostate, Str, Str, std::vector<Str>)>("PushToView");
	const auto setViewInt =
		vm->GetNative<void (*)(IVM&, std::uint32_t, std::monostate, Str, Str, std::int32_t)>("SetViewInt");
	const auto setViewStrings =
		vm->GetNative<void (*)(IVM&, std::uint32_t, std::monostate, Str, Str, std::vector<Str>)>("SetViewStrings");
	const auto listenActions =
		vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate, ObjPtr, Str)>("ListenForViewActions");
	const auto listenRequests =
		vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate, ObjPtr, Str)>("ListenForViewRequests");
	const auto replyViewInt =
		vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate, Str, std::int32_t)>("ReplyViewInt");
	const auto rejectViewRequest =
		vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate, Str, Str, Str)>("RejectViewRequest");
	const auto openMenu =
		vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate, Str)>("OpenMenu");
	const auto closeMenu =
		vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate, Str)>("CloseMenu");

	// --- registration validation ------------------------------------------------
	CHECK(registerStatic(*vm, 0, {}, "", "OnUIAction", "t.alpha") == 0);          // empty script
	CHECK(registerStatic(*vm, 0, {}, "MyLib", "", "t.alpha") == 0);               // empty function
	CHECK(registerStatic(*vm, 0, {}, "MyLib", "OnUIAction", "") == 0);            // empty mod id
	CHECK(registerStatic(*vm, 0, {}, "MyLib", "OnUIAction", "notdotted") == 0);   // dotless non-built-in
	CHECK(registerStatic(*vm, 0, {}, "MyLib", "OnUIAction", "two..dots") == 0);   // grammar violation
	CHECK(registerInstance(*vm, 0, {}, ObjPtr{}, "OnUIAction", "t.alpha") == 0);  // null receiver

	// Interned casing folds to the grammar's lowercase and is accepted.
	const auto tokenStatic = registerStatic(*vm, 0, {}, "MyLib", "OnUIAction", "T.Alpha");
	CHECK(tokenStatic != 0);

	// --- menu ids from BSFixedString are canonicalized before lookup ------------
	API::BridgeApi::Get().SetViewCatalog({ "mixed.case/view" });
	CHECK(openMenu(*vm, 0, {}, "MiXeD.CaSe/View"));
	CHECK(!openMenu(*vm, 0, {}, "MiXeD.CaSe/Missing"));
	CHECK(!closeMenu(*vm, 0, {}, "MiXeD.CaSe/View"));  // discovered but not loaded
	{
		const auto requests = API::BridgeApi::Get().TakeMenuRequests();
		CHECK(requests.size() == 1);
		if (requests.size() == 1) {
			CHECK(requests[0].view == "mixed.case/view");
			CHECK(requests[0].open);
		}
	}
	API::BridgeApi::Get().SetSurfaceLoaded("mixed.case/view", true);
	CHECK(closeMenu(*vm, 0, {}, "MiXeD.CaSe/View"));
	{
		const auto requests = API::BridgeApi::Get().TakeMenuRequests();
		CHECK(requests.size() == 1 && requests[0].view == "mixed.case/view" && !requests[0].open);
	}

	// --- static dispatch + case-insensitive mod filter ---------------------------
	vm->calls.clear();
	API::Papyrus::OnViewAction("t.alpha", "sort", { "5" });
	CHECK(vm->calls.size() == 1);
	if (vm->calls.size() == 1) {
		const auto& c = vm->calls[0];
		CHECK(c.isStatic);
		CHECK(c.scriptName == "MyLib");
		CHECK(c.fn == "OnUIAction");
		CHECK((c.args == std::vector<std::string>{ "sort", "5" }));
	}

	// Another mod's action never reaches this registration.
	vm->calls.clear();
	API::Papyrus::OnViewAction("t.beta", "sort", { "" });
	CHECK(vm->calls.empty());

	// Schema-owned hotkey callbacks queue directly and never enter the
	// session-scoped registration table.
	vm->calls.clear();
	CHECK(API::Papyrus::DispatchStaticHotkey("MyMod_Hotkeys", "OnHotkey", "t.alpha", "startScene") ==
		API::Papyrus::StaticDispatchResult::kQueued);
	CHECK(vm->calls.size() == 1);
	if (vm->calls.size() == 1) {
		const auto& c = vm->calls[0];
		CHECK(c.isStatic && c.scriptName == "MyMod_Hotkeys" && c.fn == "OnHotkey");
		CHECK((c.args == std::vector<std::string>{ "t.alpha", "startScene" }));
	}
	vm->staticDispatchSucceeds = false;
	vm->calls.clear();
	CHECK(API::Papyrus::DispatchStaticHotkey("Missing", "OnHotkey", "t.alpha", "startScene") ==
		API::Papyrus::StaticDispatchResult::kTargetRejected);
	CHECK(vm->calls.empty());
	vm->staticDispatchSucceeds = true;

	// --- kind isolation: kAction vs kSettings ------------------------------------
	const auto settingsToken = registerSettingsStatic(*vm, 0, {}, "MyLib", "OnSettingChanged", "t.alpha");
	CHECK(settingsToken != 0);

	vm->calls.clear();
	API::Papyrus::OnViewAction("t.alpha", "go", { "" });
	CHECK(vm->calls.size() == 1);  // the action registration only
	if (!vm->calls.empty()) {
		CHECK(vm->calls[0].fn == "OnUIAction");
	}

	vm->calls.clear();
	API::Papyrus::OnSettingChanged("t.alpha", "enabled");
	CHECK(vm->calls.size() == 1);  // the settings registration only
	if (!vm->calls.empty()) {
		CHECK(vm->calls[0].fn == "OnSettingChanged");
		CHECK((vm->calls[0].args == std::vector<std::string>{ "t.alpha", "enabled" }));
	}
	CHECK(unregister(*vm, 0, {}, settingsToken));

	// --- instance receiver dispatch ----------------------------------------------
	const auto receiverObj = std::make_shared<RE::BSScript::Object>();
	const auto tokenInstance = registerInstance(*vm, 0, {}, ObjPtr{ receiverObj }, "OnUIAction", "t.alpha");
	CHECK(tokenInstance != 0);

	vm->calls.clear();
	API::Papyrus::OnViewAction("T.ALPHA", "toggle", { "slot3" });  // caller casing is folded by the filter too
	CHECK(vm->calls.size() == 2);
	bool sawMethodCall = false;
	for (const auto& c : vm->calls) {
		if (!c.isStatic) {
			sawMethodCall = c.receiver == receiverObj.get();
			CHECK((c.args == std::vector<std::string>{ "toggle", "slot3" }));
		}
	}
	CHECK(sawMethodCall);

	// --- Unregister ---------------------------------------------------------------
	CHECK(unregister(*vm, 0, {}, tokenInstance));
	CHECK(!unregister(*vm, 0, {}, tokenInstance));  // stale token
	CHECK(!unregister(*vm, 0, {}, 0));              // 0 is the documented failure token
	vm->calls.clear();
	API::Papyrus::OnViewAction("t.alpha", "x", { "" });
	CHECK(vm->calls.size() == 1);  // only the static registration remains

	// --- fixed-name common listener ------------------------------------------------
	const auto simpleReceiver = std::make_shared<RE::BSScript::Object>();
	const auto simpleToken = listenActions(*vm, 0, {}, ObjPtr{ simpleReceiver }, "t.simple");
	CHECK(simpleToken != 0);
	vm->calls.clear();
	API::Papyrus::OnViewAction("t.simple", "equip", { "42", "2" });
	CHECK(vm->calls.size() == 1);
	if (!vm->calls.empty()) {
		CHECK(vm->calls[0].fn == "OnOSFUIViewAction");
		CHECK((vm->calls[0].args == std::vector<std::string>{ "equip", "42", "2" }));
	}
	CHECK(unregister(*vm, 0, {}, simpleToken));
	// --- correlated owning-Papyrus requests ---------------------------------------
	const auto requestReceiver = std::make_shared<RE::BSScript::Object>();
	const auto requestToken = listenRequests(*vm, 0, {}, ObjPtr{ requestReceiver }, "t.requests");
	CHECK(requestToken != 0);
	CHECK(listenRequests(*vm, 0, {}, ObjPtr{ requestReceiver }, "T.REQUESTS") == 0);  // first wins
	vm->calls.clear();
	CHECK(API::Papyrus::OnViewRequest("t.requests", "calculate", { "4", "true" },
		"t.requests/view", "q1"));
	CHECK(vm->calls.size() == 1);
	std::string replyToken;
	if (!vm->calls.empty()) {
		CHECK(vm->calls[0].fn == "OnOSFUIViewRequest");
		CHECK(vm->calls[0].args.size() == 4);
		CHECK(vm->calls[0].args[0] == "calculate");
		replyToken = vm->calls[0].args.back();
	}
	CHECK(!replyToken.empty());
	CHECK(replyViewInt(*vm, 0, {}, replyToken.c_str(), 77));
	CHECK(!replyViewInt(*vm, 0, {}, replyToken.c_str(), 88));  // one-shot
	std::vector<API::Papyrus::ViewReply> replies;
	API::Papyrus::DrainViewReplies([&](const auto& reply) { replies.push_back(reply); });
	CHECK(replies.size() == 1);
	if (!replies.empty()) {
		CHECK(replies[0].view == "t.requests/view" && replies[0].requestId == "q1");
		CHECK(!replies[0].rejected && replies[0].value == 77);
	}

	vm->calls.clear();
	CHECK(API::Papyrus::OnViewRequest("t.requests", "fail", {}, "t.requests/view", "q2"));
	replyToken = vm->calls[0].args.back();
	CHECK(rejectViewRequest(*vm, 0, {}, replyToken.c_str(), "not-allowed", "No"));
	replies.clear();
	API::Papyrus::DrainViewReplies([&](const auto& reply) { replies.push_back(reply); });
	CHECK(replies.size() == 1 && replies[0].rejected && replies[0].code == "not-allowed");

	vm->calls.clear();
	CHECK(API::Papyrus::OnViewRequest("t.requests", "slow", {}, "t.requests/view", "q3"));
	replies.clear();
	API::Papyrus::DrainViewReplies([&](const auto& reply) { replies.push_back(reply); },
		std::chrono::steady_clock::now() + std::chrono::seconds(11));
	CHECK(replies.size() == 1 && replies[0].rejected && replies[0].code == "papyrus-timeout");
	CHECK(!API::Papyrus::OnViewRequest("other.mod", "x", {}, "other.mod/view", "q4"));
	// --- push queue / drain --------------------------------------------------------
	std::vector<API::Papyrus::ViewPush> drained;
	const auto drain = [&] {
		API::Papyrus::DrainViewPushes([&](const API::Papyrus::ViewPush& a_push) { drained.push_back(a_push); });
	};

	pushToView(*vm, 0, {}, "T.Alpha", "slots", { Str{ "weapons" }, Str{ "aid" } });
	drain();
	CHECK(drained.size() == 1);
	if (drained.size() == 1) {
		CHECK(drained[0].mod == "t.alpha");  // folded to canonical lowercase
		CHECK(drained[0].key == "slots");
		CHECK((drained[0].values == std::vector<std::string>{ "weapons", "aid" }));
	}

	// Drained means gone.
	drained.clear();
	drain();
	CHECK(drained.empty());

	// An empty values array still delivers (it means "the list is now empty").
	pushToView(*vm, 0, {}, "t.alpha", "slots", {});
	drain();
	CHECK(drained.size() == 1);
	if (!drained.empty()) {
		CHECK(drained[0].values.empty());
	}

	// Retained typed state uses data.state's single JSON value and replays the
	// latest value without asking Papyrus to handle a page-level ready action.
	drained.clear();
	setViewInt(*vm, 0, {}, "T.Alpha", "Count", 42);
	setViewStrings(*vm, 0, {}, "t.alpha", "Names", { Str{ "ore" }, Str{ "aid" } });
	drain();
	CHECK(drained.size() == 2);
	CHECK(drained[0].stateValue && *drained[0].stateValue == 42);
	CHECK(drained[1].stateValue && *drained[1].stateValue == nlohmann::json({ "ore", "aid" }));

	std::vector<API::Papyrus::ViewPush> replayed;
	API::Papyrus::ReplayViewState("t.alpha", [&](const auto& state) { replayed.push_back(state); });
	CHECK(replayed.size() == 2);
	// The same key with different interned casing replaces, rather than forks,
	// the retained cache entry.
	setViewInt(*vm, 0, {}, "t.alpha", "COUNT", 99);
	drained.clear();
	drain();
	replayed.clear();
	API::Papyrus::ReplayViewState("T.ALPHA", [&](const auto& state) { replayed.push_back(state); });
	CHECK(replayed.size() == 2);
	bool saw99 = false;
	for (const auto& state : replayed) {
		if (state.key == "COUNT") {
			saw99 = state.stateValue && *state.stateValue == 99;
		}
	}
	CHECK(saw99);
	// Invalid mod id / empty key are refused with a WARN, nothing queued.
	drained.clear();
	pushToView(*vm, 0, {}, "notdotted", "slots", { Str{ "x" } });
	pushToView(*vm, 0, {}, "t.alpha", "", { Str{ "x" } });
	drain();
	CHECK(drained.empty());
	CHECK(LogCount("PushToView") >= 2);  // both refusals logged

	// Drop-newest cap: only the first 1024 queued pushes survive.
	drained.clear();
	for (int i = 0; i < 1100; ++i) {
		pushToView(*vm, 0, {}, "t.alpha", "k", { Str{ "v" } });
	}
	CHECK(LogCount("view-push queue full") > 0);
	drain();
	CHECK(drained.size() == 1024);

	// --- load-game teardown ---------------------------------------------------------
	// The TESLoadGameEvent sink clears every registration (session scope) and
	// re-binds the natives on the (rebuilt) VM.
	vm->natives.clear();
	RE::TESLoadGameEvent::GetEventSource()->Notify(RE::TESLoadGameEvent{});
	CHECK(vm->natives.contains("RegisterForViewActions"));  // re-bound
	vm->calls.clear();
	API::Papyrus::OnViewAction("t.alpha", "sort", { "1" });
	CHECK(vm->calls.empty());                    // registrations gone
	CHECK(!unregister(*vm, 0, {}, tokenStatic));  // pre-load token never validates again
	CHECK(API::Papyrus::DispatchStaticHotkey("MyMod_Hotkeys", "OnHotkey", "t.alpha", "startScene") ==
		API::Papyrus::StaticDispatchResult::kQueued);
	CHECK(vm->calls.size() == 1 && vm->calls[0].scriptName == "MyMod_Hotkeys");
	replayed.clear();
	API::Papyrus::ReplayViewState("t.alpha", [&](const auto& state) { replayed.push_back(state); });
	CHECK(replayed.empty());  // session-scoped state (including form ids) is gone

	// A fresh post-load registration works (generations stayed monotonic).
	const auto tokenAfterLoad = registerStatic(*vm, 0, {}, "MyLib", "OnUIAction", "t.alpha");
	CHECK(tokenAfterLoad != 0);
	CHECK(tokenAfterLoad != tokenStatic);
	vm->calls.clear();
	API::Papyrus::OnViewAction("t.alpha", "ready", { "" });
	CHECK(vm->calls.size() == 1);
	CHECK(unregister(*vm, 0, {}, tokenAfterLoad));

	// --- args-list shape (RegisterForViewActionsArgs) ----------------------------
	// A view sends `args: [...]`; the whole list reaches the callback as a
	// Papyrus string[] (the stub's recording VM flattens the packed array back
	// into the call's args, after the leading action string).
	const auto argsStatic = registerArgsStatic(*vm, 0, {}, "ArgsLib", "OnUIAction", "t.delta");
	CHECK(argsStatic != 0);
	vm->calls.clear();
	API::Papyrus::OnViewAction("t.delta", "untrack", { "1", "7" });
	CHECK(vm->calls.size() == 1);
	if (vm->calls.size() == 1) {
		CHECK(vm->calls[0].isStatic);
		CHECK(vm->calls[0].scriptName == "ArgsLib");
		CHECK((vm->calls[0].args == std::vector<std::string>{ "untrack", "1", "7" }));  // action + list
	}

	// No-arg action to an args-list registrant delivers an empty list.
	vm->calls.clear();
	API::Papyrus::OnViewAction("t.delta", "ready", {});
	CHECK(vm->calls.size() == 1);
	if (!vm->calls.empty()) {
		CHECK((vm->calls[0].args == std::vector<std::string>{ "ready" }));  // action only, empty list
	}

	// Mixed shapes on one mod: the scalar registrant gets args[0], the args-list
	// registrant gets the whole list — each in the form it registered for.
	const auto scalarOnDelta = registerStatic(*vm, 0, {}, "ScalarLib", "OnUIAction", "t.delta");
	CHECK(scalarOnDelta != 0);
	vm->calls.clear();
	API::Papyrus::OnViewAction("t.delta", "edit", { "3", "tag" });
	CHECK(vm->calls.size() == 2);
	bool sawScalar = false, sawArgs = false;
	for (const auto& c : vm->calls) {
		if (c.scriptName == "ScalarLib") {
			sawScalar = (c.args == std::vector<std::string>{ "edit", "3" });  // action + first element
		} else if (c.scriptName == "ArgsLib") {
			sawArgs = (c.args == std::vector<std::string>{ "edit", "3", "tag" });  // action + whole list
		}
	}
	CHECK(sawScalar);
	CHECK(sawArgs);

	// Instance args-list variant registers and dispatches too.
	const auto argsReceiver = std::make_shared<RE::BSScript::Object>();
	const auto argsInstance = registerArgsInstance(*vm, 0, {}, ObjPtr{ argsReceiver }, "OnUIAction", "t.epsilon");
	CHECK(argsInstance != 0);
	vm->calls.clear();
	API::Papyrus::OnViewAction("t.epsilon", "rename", { "0", "5" });
	CHECK(vm->calls.size() == 1);
	if (!vm->calls.empty()) {
		CHECK(!vm->calls[0].isStatic);
		CHECK(vm->calls[0].receiver == argsReceiver.get());
		CHECK((vm->calls[0].args == std::vector<std::string>{ "rename", "0", "5" }));
	}

	std::fprintf(stderr, "papyrus_action_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures;
}
