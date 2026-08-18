// Native desktop tests for the Papyrus dynamic-data API (protocol 2.0,
// docs/mod-api-2.0-design.md): the REAL API/PapyrusApi.cpp and
// Bridge/RetainedStateStore.cpp compiled against stubs/RE (a recording VM),
// driven through the same natives the game binds.
//
// 2.0 replaced the single transient `PushToView` channel with the state/event
// pair; the temporary v1 adapter keeps that old channel through 2.0.x. SetView* is RETAINED
// state: latest-wins, complete per key, held in the shared RetainedStateStore and
// replayed to every document that greets the bridge — which is why a view
// survives F5 with no handshake. SendViewEvent is a one-shot happening:
// delivered at most once, never stored, never replayed. Encoding one as the
// other is the blank-after-reload bug in one direction and the
// event-refires-on-every-reload bug in the other, so both halves are asserted
// against the same store the OSF UI runtime replays from.
//
// Also covers the action-dispatch registry (case-insensitive mod filter,
// static/instance targets, token release, kind isolation), correlated view
// requests, and the load-game teardown that drops both queues and raises the
// one-shot session-reset flag.
// Assert-style; process exit code is the failure count.

#include "API/BridgeApi.h"
#include "API/PapyrusApi.h"
#include "Compat/V1/Papyrus.h"
#include "Core/StringUtil.h"
#include "Bridge/RetainedStateStore.h"

#include "RE/B/BSScriptUtil.h"
#include "RE/E/Events.h"
#include "check.h"

namespace
{

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

// Core/Log.h declarations (real impl pulls game deps — stub, as in
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
	CHECK(vm->natives.contains("SetViewInt"));
	CHECK(vm->natives.contains("SetViewStrings"));
	CHECK(vm->natives.contains("SendViewEvent"));
	CHECK(vm->natives.contains("ListenForViewActions"));
	CHECK(vm->natives.contains("ListenForViewActionsStatic"));
	CHECK(vm->natives.contains("ListenForViewRequests"));
	CHECK(vm->natives.contains("ReplyViewInt"));
	CHECK(vm->natives.contains("Unregister"));

	CHECK(vm->natives.contains("PushToView"));
	CHECK(vm->natives.contains("PushFormsToView"));
	CHECK(vm->natives.contains("RegisterForViewActions"));
	CHECK(vm->natives.contains("RegisterForViewActionsStatic"));
	CHECK(vm->natives.contains("RegisterForViewActionsArgs"));
	CHECK(vm->natives.contains("RegisterForViewActionsArgsStatic"));

	const auto listenStatic =
		vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate, Str, Str)>("ListenForViewActionsStatic");
	const auto listenInstance =
		vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate, ObjPtr, Str)>("ListenForViewActions");
	const auto registerSettingsStatic =
		vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate, Str, Str, Str)>("RegisterForSettingChangesStatic");
	const auto unregister =
		vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate, std::int32_t)>("Unregister");
	const auto setViewInt =
		vm->GetNative<void (*)(IVM&, std::uint32_t, std::monostate, Str, Str, std::int32_t)>("SetViewInt");
	const auto setViewStrings =
		vm->GetNative<void (*)(IVM&, std::uint32_t, std::monostate, Str, Str, std::vector<Str>)>("SetViewStrings");
	const auto sendViewEvent =
		vm->GetNative<void (*)(IVM&, std::uint32_t, std::monostate, Str, Str, std::vector<Str>)>("SendViewEvent");
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
	const auto registerLegacyStatic =
		vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate, Str, Str, Str)>("RegisterForViewActionsStatic");
	const auto registerLegacyArgsStatic =
		vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate, Str, Str, Str)>("RegisterForViewActionsArgsStatic");
	const auto pushToView =
		vm->GetNative<void (*)(IVM&, std::uint32_t, std::monostate, Str, Str, std::vector<Str>)>("PushToView");

	// --- registration validation ------------------------------------------------
	// The four RegisterForViewActions* shapes collapsed into one listener per
	// target kind, so the callback name is fixed and only the target and the mod
	// id are still script-supplied — those are what can still be wrong.
	CHECK(listenStatic(*vm, 0, {}, "", "t.alpha") == 0);              // empty script
	CHECK(listenStatic(*vm, 0, {}, "MyLib", "") == 0);                // empty mod id
	CHECK(listenStatic(*vm, 0, {}, "MyLib", "../evil") == 0);         // path separator
	CHECK(listenStatic(*vm, 0, {}, "MyLib", "osfui") == 0);          // platform-reserved
	CHECK(listenInstance(*vm, 0, {}, ObjPtr{}, "t.alpha") == 0);      // null receiver
	const auto legacyScalar = registerLegacyStatic(*vm, 0, {}, "LegacyLib", "OnLegacy", "T.Legacy");
	const auto legacyArgs = registerLegacyArgsStatic(*vm, 0, {}, "LegacyLib", "OnLegacyArgs", "T.Legacy");
	CHECK(legacyScalar != 0 && legacyArgs != 0);
	vm->calls.clear();
	API::Papyrus::OnViewAction("t.legacy", "sort", { "aid", "ammo" });
	CHECK(vm->calls.size() == 2);
	for (const auto& call : vm->calls) {
		if (call.fn == "OnLegacy") CHECK((call.args == std::vector<std::string>{ "sort", "aid" }));
		if (call.fn == "OnLegacyArgs") CHECK((call.args == std::vector<std::string>{ "sort", "aid", "ammo" }));
	}
	CHECK(unregister(*vm, 0, {}, legacyScalar));
	CHECK(unregister(*vm, 0, {}, legacyArgs));

	pushToView(*vm, 0, {}, "T.Legacy", "Inventory", { "aid", "ammo" });
	std::vector<Compat::V1::Papyrus::Push> pushes;
	Compat::V1::Papyrus::DrainPushes([&](const auto& push) { pushes.push_back(push); });
	CHECK(pushes.size() == 1);
	if (!pushes.empty()) {
		CHECK(pushes[0].mod == "t.legacy");
		CHECK(pushes[0].payload["key"] == "Inventory");
		CHECK(pushes[0].payload["values"] == (nlohmann::json::array({ "aid", "ammo" })));
	}
	pushToView(*vm, 0, {}, "T.Legacy", "Discarded", { "stale" });
	Compat::V1::Papyrus::ClearPendingPushes();
	pushes.clear();
	Compat::V1::Papyrus::DrainPushes([&](const auto& push) { pushes.push_back(push); });
	CHECK(pushes.empty());
	const auto legacyCallers = Compat::V1::Papyrus::TakeCallers();
	CHECK(legacyCallers.size() == 1);
	if (!legacyCallers.empty()) CHECK(legacyCallers[0] == "t.legacy");
	// Interned ASCII casing folds to a stable comparison form and is accepted.
	const auto tokenStatic = listenStatic(*vm, 0, {}, "MyLib", "T.Alpha");
	CHECK(tokenStatic != 0);

	// --- menu ids from BSFixedString are canonicalized before lookup ------------
	API::BridgeApi::Get().SetViewCatalog({ "mixed.case/view" });
	CHECK(openMenu(*vm, 0, {}, "MiXeD.CaSe/View"));
	CHECK(!openMenu(*vm, 0, {}, "MiXeD.CaSe/Missing"));
	CHECK(!closeMenu(*vm, 0, {}, "MiXeD.CaSe/View"));  // discovered but not instantiated
	{
		const auto requests = API::BridgeApi::Get().TakeViewPresentationRequests();
		CHECK(requests.size() == 1);
		if (requests.size() == 1) {
			CHECK(requests[0].view == "mixed.case/view");
			CHECK(requests[0].open);
		}
	}
	API::BridgeApi::Get().SetViewInstantiated("mixed.case/view", true);
	CHECK(closeMenu(*vm, 0, {}, "MiXeD.CaSe/View"));
	{
		const auto requests = API::BridgeApi::Get().TakeViewPresentationRequests();
		CHECK(requests.size() == 1 && requests[0].view == "mixed.case/view" && !requests[0].open);
	}

	// --- static dispatch + case-insensitive mod filter ---------------------------
	// The one action callback shape: OnOSFUIViewAction(string, string[]). The
	// stub VM flattens the packed string[] inline after the leading action name,
	// so a whole call reads as one flat vector.
	vm->calls.clear();
	API::Papyrus::OnViewAction("t.alpha", "sort", { "5" });
	CHECK(vm->calls.size() == 1);
	if (vm->calls.size() == 1) {
		const auto& c = vm->calls[0];
		CHECK(c.isStatic);
		CHECK(c.scriptName == "MyLib");
		CHECK(c.fn == "OnOSFUIViewAction");
		CHECK((c.args == std::vector<std::string>{ "sort", "5" }));
	}

	// Another mod's action never reaches this registration.
	vm->calls.clear();
	API::Papyrus::OnViewAction("t.beta", "sort", { "" });
	CHECK(vm->calls.empty());

	// A no-arg message delivers an EMPTY list, not a one-element list holding "".
	vm->calls.clear();
	API::Papyrus::OnViewAction("t.alpha", "ready", {});
	CHECK(vm->calls.size() == 1);
	if (!vm->calls.empty()) {
		CHECK((vm->calls[0].args == std::vector<std::string>{ "ready" }));
	}

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

	// A view can call any GLOBAL function on a loose PEX without a quest record
	// or session registration. Scalar types remain native Papyrus types.
	vm->calls.clear();
	CHECK(API::Papyrus::DispatchStaticFunction("RecordlessBackend", "Equip",
		{ std::string("ff012345"), std::int32_t(2), 1.5f, true }) ==
		API::Papyrus::StaticDispatchResult::kQueued);
	CHECK(vm->calls.size() == 1);
	if (vm->calls.size() == 1) {
		const auto& c = vm->calls[0];
		CHECK(c.isStatic && c.scriptName == "RecordlessBackend" && c.fn == "Equip");
		CHECK((c.args == std::vector<std::string>{ "ff012345", "2", "1.500000", "true" }));
		CHECK((c.argTypes == std::vector<std::string>{ "string", "int", "float", "bool" }));
	}
	vm->staticDispatchSucceeds = false;
	vm->calls.clear();
	CHECK(API::Papyrus::DispatchStaticFunction("Missing", "Equip", {}) ==
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
		CHECK(vm->calls[0].fn == "OnOSFUIViewAction");
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
	// Two listeners on one mod (one global target, one instance) both fire, each
	// through its own dispatch path, with identical arguments.
	const auto receiverObj = std::make_shared<RE::BSScript::Object>();
	const auto tokenInstance = listenInstance(*vm, 0, {}, ObjPtr{ receiverObj }, "t.alpha");
	CHECK(tokenInstance != 0);

	vm->calls.clear();
	API::Papyrus::OnViewAction("T.ALPHA", "toggle", { "slot3", "on" });  // caller casing is folded by the filter too
	CHECK(vm->calls.size() == 2);
	bool sawMethodCall = false;
	for (const auto& c : vm->calls) {
		CHECK((c.args == std::vector<std::string>{ "toggle", "slot3", "on" }));
		if (!c.isStatic) {
			sawMethodCall = c.receiver == receiverObj.get();
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
		CHECK(replies[0].view == "t.requests/view" && replies[0].deferToken == "q1");
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

	// --- the runtime's tick, in miniature ------------------------------------------
	// Runtime::Tick drains both queues into the SAME shared RetainedStateStore
	// decision: state is retained there and delivered; an event is only
	// delivered. Every "will a fresh document see it?" assertion below reads the
	// store, because the store is the only thing a greeting document is replayed
	// from. TakeSessionReset is deliberately NOT consumed here — the load-game
	// section asserts on it directly.
	RetainedStateStore                   store;
	std::vector<API::Papyrus::ViewState> states;
	std::vector<API::Papyrus::ViewEvent> events;
	const auto                           tick = [&] {
		states.clear();
		events.clear();
		API::Papyrus::DrainViewState([&](const API::Papyrus::ViewState& a_state) {
			store.Set(a_state.mod, a_state.key, a_state.value, /*sessionScoped*/ true);
			states.push_back(a_state);
		});
		API::Papyrus::DrainViewEvents([&](const API::Papyrus::ViewEvent& a_event) {
			events.push_back(a_event);
		});
	};

	// --- SetView* state queue / drain ----------------------------------------------
	setViewStrings(*vm, 0, {}, "T.Alpha", "slots", { Str{ "weapons" }, Str{ "aid" } });
	tick();
	CHECK(states.size() == 1);
	if (states.size() == 1) {
		CHECK(states[0].mod == "t.alpha");  // folded to canonical lowercase
		CHECK(states[0].key == "slots");
		CHECK(states[0].value == nlohmann::json({ "weapons", "aid" }));
	}

	// Drained means gone: the queue is a handoff to the main thread, not a log.
	tick();
	CHECK(states.empty());

	// An empty array still delivers — it is the complete value, and it means
	// "the list is now empty" rather than "nothing changed".
	setViewStrings(*vm, 0, {}, "t.alpha", "slots", {});
	tick();
	CHECK(states.size() == 1);
	if (!states.empty()) {
		CHECK(states[0].value.is_array() && states[0].value.empty());
	}

	// Each typed setter delivers ONE complete JSON value for its key, never a
	// delta, which is what makes a replay and a live update the same message.
	setViewInt(*vm, 0, {}, "T.Alpha", "Count", 42);
	setViewStrings(*vm, 0, {}, "t.alpha", "Names", { Str{ "ore" }, Str{ "aid" } });
	tick();
	CHECK(states.size() == 2);
	if (states.size() == 2) {
		CHECK(states[0].value == 42);
		CHECK(states[1].value == nlohmann::json({ "ore", "aid" }));
	}

	// --- retention: what a fresh document is replayed --------------------------------
	{
		const auto* replay = store.Find("t.alpha");
		CHECK(replay != nullptr);
		// slots, Count, Names — in insertion order, so a replay reads like the
		// publisher's own sequence.
		CHECK(replay && replay->size() == 3);
		if (replay && replay->size() == 3) {
			CHECK((*replay)[0].key == "slots");
			CHECK((*replay)[1].key == "Count" && (*replay)[1].value == 42);
			CHECK((*replay)[2].key == "Names");
		}
	}
	// Publishing the same key again REPLACES the retained value rather than
	// forking it, even spelled differently: a Papyrus key arrives through
	// BSFixedString interning, which hands back the first-seen casing
	// process-wide, so the script's literal spelling is not a reliable identity.
	setViewInt(*vm, 0, {}, "t.alpha", "COUNT", 99);
	tick();
	CHECK(states.size() == 1);
	{
		const auto* replay = store.Find("T.ALPHA");  // the mod id folds too
		CHECK(replay && replay->size() == 3);        // replaced, not appended
		bool saw99 = false;
		if (replay) {
			for (const auto& entry : *replay) {
				if (StringUtil::EqualsCaseInsensitiveAscii(entry.key, "count")) {
					saw99 = entry.value == 99;
				}
			}
		}
		CHECK(saw99);
	}

	// Invalid mod id / empty key are refused with a WARN naming the native the
	// script actually called; nothing is queued.
	setViewStrings(*vm, 0, {}, "../evil", "slots", { Str{ "x" } });
	setViewStrings(*vm, 0, {}, "t.alpha", "", { Str{ "x" } });
	tick();
	CHECK(states.empty());
	CHECK(LogCount("SetViewStrings") >= 2);  // both refusals logged

	// Drop-newest cap: with the runtime disabled via config the drain never
	// runs, so a scripted Set loop must not grow the process forever.
	for (int i = 0; i < 1100; ++i) {
		setViewStrings(*vm, 0, {}, "t.alpha", "k", { Str{ "v" } });
	}
	CHECK(LogCount("view-state queue full") > 0);
	tick();
	CHECK(states.size() == 1024);

	// --- SendViewEvent: one-shot, never retained, never replayed ---------------------
	sendViewEvent(*vm, 0, {}, "T.Alpha", "ScanFinished", { Str{ "3" }, Str{ "ore" } });
	tick();
	CHECK(events.size() == 1);
	if (events.size() == 1) {
		CHECK(events[0].mod == "t.alpha");        // folded exactly like state
		CHECK(events[0].name == "ScanFinished");  // the author's spelling reaches the view
		CHECK((events[0].args == std::vector<std::string>{ "3", "ore" }));
	}
	CHECK(states.empty());  // an event is not state and never becomes one
	// ...and nothing about it entered the retained store, so no document that
	// opens later can be replayed it. THIS is the whole distinction between the
	// two verbs: replaying a happening re-fires its effect.
	{
		const auto* replay = store.Find("t.alpha");
		CHECK(replay != nullptr);
		if (replay) {
			for (const auto& entry : *replay) {
				CHECK(entry.key != "ScanFinished");
			}
		}
	}
	tick();
	CHECK(events.empty());  // delivered at most once

	// An event with no args carries an empty list, never a one-element list
	// holding "" — the view reads payload.args and must not see a phantom entry.
	sendViewEvent(*vm, 0, {}, "t.alpha", "pulse", {});
	tick();
	CHECK(events.size() == 1 && events[0].args.empty());

	// Same target validation as state, and the refusal names SendViewEvent.
	sendViewEvent(*vm, 0, {}, "../evil", "x", { Str{ "1" } });
	sendViewEvent(*vm, 0, {}, "t.alpha", "", { Str{ "1" } });
	tick();
	CHECK(events.empty());
	CHECK(LogCount("SendViewEvent") >= 2);

	// Same drop-newest cap, for the same reason.
	for (int i = 0; i < 1100; ++i) {
		sendViewEvent(*vm, 0, {}, "t.alpha", "spam", {});
	}
	CHECK(LogCount("view-event queue full") > 0);
	tick();
	CHECK(events.size() == 1024);

	// --- load-game teardown ----------------------------------------------------------
	// The TESLoadGameEvent sink clears every registration (session scope),
	// re-binds the natives on the (rebuilt) VM, DROPS both queues — their values
	// can hold form identities belonging to the session that just ended — and
	// raises the one-shot flag the next tick consumes.
	setViewInt(*vm, 0, {}, "t.alpha", "count", 5);
	sendViewEvent(*vm, 0, {}, "t.alpha", "boom", {});
	CHECK(!API::Papyrus::TakeSessionReset());  // nothing to report before a load
	vm->natives.clear();
	RE::TESLoadGameEvent::GetEventSource()->Notify(RE::TESLoadGameEvent{});
	CHECK(vm->natives.contains("ListenForViewActions"));  // re-bound
	tick();
	CHECK(states.empty() && events.empty());   // the queues were dropped, not delivered
	CHECK(API::Papyrus::TakeSessionReset());   // raised once by the load...
	CHECK(!API::Papyrus::TakeSessionReset());  // ...and consumed by the tick that saw it
	store.ClearSessionScoped();                // what Runtime::Tick does with the flag
	CHECK(store.Find("t.alpha") == nullptr);   // retained Papyrus state is gone with it

	vm->calls.clear();
	API::Papyrus::OnViewAction("t.alpha", "sort", { "1" });
	CHECK(vm->calls.empty());                     // registrations gone
	CHECK(!unregister(*vm, 0, {}, tokenStatic));  // pre-load token never validates again
	CHECK(API::Papyrus::DispatchStaticHotkey("MyMod_Hotkeys", "OnHotkey", "t.alpha", "startScene") ==
		API::Papyrus::StaticDispatchResult::kQueued);
	CHECK(vm->calls.size() == 1 && vm->calls[0].scriptName == "MyMod_Hotkeys");

	// A fresh post-load registration works (generations stayed monotonic).
	const auto tokenAfterLoad = listenStatic(*vm, 0, {}, "MyLib", "t.alpha");
	CHECK(tokenAfterLoad != 0);
	CHECK(tokenAfterLoad != tokenStatic);
	vm->calls.clear();
	API::Papyrus::OnViewAction("t.alpha", "ready", { "" });
	CHECK(vm->calls.size() == 1);
	CHECK(unregister(*vm, 0, {}, tokenAfterLoad));

	// --- RetainedStateStore, directly -------------------------------------------------
	// The store both mod backend types share (Papyrus SetView* and the native ABI's
	// SetViewState land here and replay by the same rule), tested apart from the
	// queues that feed it.
	{
		RetainedStateStore s;
		CHECK(!s.Set("", "k", 1));         // an empty mod id is not a target
		CHECK(!s.Set("t.store", "", 1));   // nor is an empty key
		CHECK(s.ModCount() == 0);

		CHECK(s.Set("t.store", "Alpha", 1, /*sessionScoped*/ true));
		CHECK(s.Set("t.store", "beta", "x"));
		// Latest-wins on a case-insensitively equal key, and the publisher's
		// spelling is refreshed with the value: a script that changed only the
		// casing should not keep delivering the first spelling forever.
		CHECK(s.Set("T.STORE", "ALPHA", 2, /*sessionScoped*/ true));
		{
			const auto* entries = s.Find("t.store");
			CHECK(entries && entries->size() == 2);  // replaced, not forked
			if (entries && entries->size() == 2) {
				CHECK((*entries)[0].key == "ALPHA");  // refreshed spelling
				CHECK((*entries)[0].value == 2);      // latest value
				CHECK((*entries)[0].sessionScoped);
				CHECK((*entries)[1].key == "beta");   // insertion order preserved
				CHECK((*entries)[1].value == "x");
				CHECK(!(*entries)[1].sessionScoped);  // the native ABI's half
			}
			CHECK(s.Find("T.Store") != nullptr);  // mod lookup folds case too
			CHECK(s.Find("t.missing") == nullptr);
		}

		// Per-mod key cap: a mod looping on SetView* with generated keys hits
		// this instead of growing the process without bound, and a mod with a
		// fixed key set is never affected — an ALREADY-retained key still
		// updates at capacity.
		for (std::size_t i = 0; i < RetainedStateStore::kMaxKeysPerMod; ++i) {
			CHECK(s.Set("t.capped", "k" + std::to_string(i), static_cast<int>(i), true));
		}
		const auto warnsBefore = LogCount("holds the maximum");
		CHECK(!s.Set("t.capped", "one-too-many", 0, true));  // delivered live, not retained
		CHECK(LogCount("holds the maximum") == warnsBefore + 1);
		CHECK(s.Set("t.capped", "K0", 999, true));  // an existing key always updates
		{
			const auto* capped = s.Find("t.capped");
			CHECK(capped && capped->size() == RetainedStateStore::kMaxKeysPerMod);
			CHECK(capped && (*capped)[0].value == 999);
		}

		// A game load drops only the session-scoped half. Wiping a native
		// plugin's HUD config on every load would be the bug, so the scope
		// travels with the entry rather than being one policy for the store.
		s.ClearSessionScoped();
		{
			const auto* entries = s.Find("t.store");
			CHECK(entries && entries->size() == 1);
			if (entries && entries->size() == 1) {
				CHECK((*entries)[0].key == "beta");
			}
			// A mod left with nothing drops out entirely rather than lingering
			// as an empty bucket.
			CHECK(s.Find("t.capped") == nullptr);
			CHECK(s.ModCount() == 1);
		}

		s.RemoveMod("T.STORE");  // its plugin unloaded / its schema went away
		CHECK(s.Find("t.store") == nullptr);
		CHECK(s.ModCount() == 0);

		CHECK(s.Set("t.store", "again", true));
		s.Clear();  // shutdown / renderer teardown
		CHECK(s.ModCount() == 0);
		CHECK(s.Find("t.store") == nullptr);
	}

	std::fprintf(stderr, "papyrus_action_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures;
}
