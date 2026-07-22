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
	CHECK(vm->natives.contains("RegisterForViewActions"));
	CHECK(vm->natives.contains("RegisterForViewActionsStatic"));
	CHECK(vm->natives.contains("Unregister"));

	const auto registerStatic =
		vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate, Str, Str, Str)>("RegisterForViewActionsStatic");
	const auto registerInstance =
		vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate, ObjPtr, Str, Str)>("RegisterForViewActions");
	const auto registerSettingsStatic =
		vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate, Str, Str, Str)>("RegisterForSettingChangesStatic");
	const auto unregister =
		vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate, std::int32_t)>("Unregister");
	const auto pushToView =
		vm->GetNative<void (*)(IVM&, std::uint32_t, std::monostate, Str, Str, std::vector<Str>)>("PushToView");
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
	API::Papyrus::OnViewAction("t.alpha", "sort", "5");
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
	API::Papyrus::OnViewAction("t.beta", "sort", "");
	CHECK(vm->calls.empty());

	// --- kind isolation: kAction vs kSettings ------------------------------------
	const auto settingsToken = registerSettingsStatic(*vm, 0, {}, "MyLib", "OnSettingChanged", "t.alpha");
	CHECK(settingsToken != 0);

	vm->calls.clear();
	API::Papyrus::OnViewAction("t.alpha", "go", "");
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
	API::Papyrus::OnViewAction("T.ALPHA", "toggle", "slot3");  // caller casing is folded by the filter too
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
	API::Papyrus::OnViewAction("t.alpha", "x", "");
	CHECK(vm->calls.size() == 1);  // only the static registration remains

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
	API::Papyrus::OnViewAction("t.alpha", "sort", "1");
	CHECK(vm->calls.empty());                    // registrations gone
	CHECK(!unregister(*vm, 0, {}, tokenStatic));  // pre-load token never validates again

	// A fresh post-load registration works (generations stayed monotonic).
	const auto tokenAfterLoad = registerStatic(*vm, 0, {}, "MyLib", "OnUIAction", "t.alpha");
	CHECK(tokenAfterLoad != 0);
	CHECK(tokenAfterLoad != tokenStatic);
	vm->calls.clear();
	API::Papyrus::OnViewAction("t.alpha", "ready", "");
	CHECK(vm->calls.size() == 1);

	std::fprintf(stderr, "papyrus_action_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures;
}
