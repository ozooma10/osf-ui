#include "API/PapyrusApi.h"
#include "API/BridgeApi.h"
#include "Bridge/MessageBridge.h"
#include "Compat/V1/PapyrusAdapter.h"
#include "Compat/V1/LegacyBridge.h"
#include "Compat/V1/SettingsCodec.h"
#include "Core/Paths.h"
#include "RE/B/BSScriptUtil.h"
#include "RE/E/Events.h"
#include "SettingsServices.h"
#include "vendor/OSFSettings_Providers.h"
#include "vendor/OSFSettingsRegistry.h"
#include "check.h"
#include <limits>
#include <thread>

namespace OSFUI::Paths
{
    const std::filesystem::path& DataDir()
    {
        static const auto path = std::filesystem::temp_directory_path() / "osfui-ssse-contract-tests" / "OSF" / "UI";
        return path;
    }
}
namespace OSFUI::Log
{
    bool DebugEnabled() { return false; }
    void SetDebugLogging(bool) {}
    void WarnOnce(std::once_flag& flag, std::string_view text) { std::call_once(flag, [&] { REX::test::Log("WARN", std::string(text)); }); }
}
namespace
{
    using namespace OSFSettings::API;
    struct Settings : SettingsTest::Settings
    {
        std::uint32_t key{0x76}; // Original openExchange = F7, Settings owns the live value.
        Status ReadRegistry(const char*, RegistryFn fn, void* context) noexcept override
        {
            SettingView setting{};
            setting.key = {"openExchange", 12}; setting.type = SettingType::Key; setting.value.key = key;
            GroupView group{}; group.settings = &setting; group.settingCount = 1;
            ModView mod{}; mod.id = {"x2357.ssse", 10}; mod.groups = &group; mod.groupCount = 1;
            fn({&mod, 1}, context);
            return Status::Ok;
        }
    } settings;
    struct Providers : OSFSettings::API::Providers::IProviders
    {
        struct Observer { HotkeyFn fn; void* context; };
        std::map<Subscription, Observer> observers;
        Subscription next{1};
        int registrations{}, subscriptions{}, removals{};
        Status Register(const char*, const char*, const char*, OSFSettings::API::Providers::SaveFn,
            void*, OSFSettings::API::Providers::Registration*) noexcept override { ++registrations; return Status::Ok; }
        Status Unregister(OSFSettings::API::Providers::Registration) noexcept override { return Status::Ok; }
        Status SubscribeKey(const char* mod, const char* key, HotkeyFn fn, void* context, Subscription* out) noexcept override
        {
            CHECK(std::string_view(mod) == "x2357.ssse");
            CHECK(std::string_view(key) == "openExchange"); // Do not lowercase Settings IDs.
            *out = next++; observers.emplace(*out, Observer{fn, context}); ++subscriptions;
            return Status::Ok;
        }
        Status UnsubscribeKey(Subscription token) noexcept override
        { ++removals; return observers.erase(token) ? Status::Ok : Status::UnknownSubscription; }
        void Press(std::uint32_t code, bool gameplay = true)
        {
            if (!gameplay || !settings.blocks.empty() || code != settings.key) return;
            for (const auto& [_, observer] : observers) observer.fn("x2357.ssse", "openExchange", observer.context);
        }
    } providers;
    void* AcquireProviders(std::uint32_t need, std::uint32_t* actual) noexcept
    { *actual = Supports(kVersion, need) ? kVersion : 0; return *actual ? &providers : nullptr; }
}

int main()
{
    using namespace OSFUI;
    namespace Legacy = Compat::V1::Papyrus;
    using Json = nlohmann::json;
    using Str = RE::BSFixedString;
    using IVM = RE::BSScript::IVirtualMachine;
    using Object = RE::BSTSmartPointer<RE::BSScript::Object>;
    SettingsTest::Install(&settings, nullptr);
    REX::W32::test::procLookup = [](void*, const char* name) noexcept -> void* {
        if (std::string_view(name) == "OSFSettings_RequestAPI") return reinterpret_cast<void*>(&SettingsTest::RequestSettings);
        if (std::string_view(name) == "OSFSettings_RequestProvidersAPI") return reinterpret_cast<void*>(&AcquireProviders);
        return nullptr;
    };
    auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    API::Papyrus::Install();
    // any_cast verifies the exact compiled native signatures, including void vs bool.
    const auto setFloats = vm->GetNative<void (*)(IVM&, std::uint32_t, std::monostate, Str, Str, std::vector<float>)>("SetViewFloats");
    const auto setInts = vm->GetNative<void (*)(IVM&, std::uint32_t, std::monostate, Str, Str, std::vector<std::int32_t>)>("SetViewInts");
    const auto listenActions = vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate, Object, Str)>("ListenForViewActions");
    const auto listenRequests = vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate, Object, Str)>("ListenForViewRequests");
    const auto registerHotkey = vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t, std::monostate, Object, Str, Str, Str)>("RegisterForHotkey");
    const auto unregister = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate, std::int32_t)>("Unregister");
    const auto replyFloats = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate, Str, std::vector<float>)>("ReplyViewFloats");
    const auto replyInts = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate, Str, std::vector<std::int32_t>)>("ReplyViewInts");
    const auto replyInt = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate, Str, std::int32_t)>("ReplyViewInt");
    const auto reject = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate, Str, Str, Str)>("RejectViewRequest");
    const auto open = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate, Str)>("OpenMenu");
    auto type = std::make_shared<RE::BSScript::ObjectTypeInfo>(); type->name = "X2357SSSE_View";
    auto object = std::make_shared<RE::BSScript::Object>(); object->type = RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo>(type); object->handle = 123;
    Object receiver(object); vm->boundObjects[123] = receiver;
    const auto action = listenActions(*vm, 0, {}, receiver, "x2357.ssse");
    auto request = listenRequests(*vm, 0, {}, receiver, "x2357.ssse");
    const auto hotkey = registerHotkey(*vm, 0, {}, receiver, "OnHotkey", "X2357.SSSE", "OPENEXCHANGE");
    CHECK(action > 0 && request > 0 && hotkey > 0);
    CHECK(listenActions(*vm, 0, {}, receiver, "x2357.ssse") == action);
    CHECK(listenRequests(*vm, 0, {}, receiver, "x2357.ssse") == 0); // first wins
    CHECK(registerHotkey(*vm, 0, {}, receiver, "OnHotkey", "x2357.ssse", "openExchange") == hotkey);
    CHECK(listenActions(*vm, 0, {}, {}, "x2357.ssse") == 0);
    CHECK(listenActions(*vm, 0, {}, receiver, "../bad") == 0);

    const std::string view = "x2357.ssse/exchange", other = "other.mod/exchange";
    std::vector<std::pair<std::string, Json>> wire;
    MessageBridge bridge([&](std::string_view target, std::string_view message) { wire.emplace_back(target, Json::parse(message)); });
    Legacy::RegisterEndpoints(bridge, [&](std::string_view id) { return id == view || id == other || id == "x2357.ssse/chart"; });
    bridge.SetHelloHook([&](std::string_view id) { Legacy::ReplayState(bridge, id); });
    const auto send = [&](std::string_view target, std::string name, Json payload = nlohmann::json::object(), std::string id = "") {
        Json envelope{{"kind", id.empty() ? "send" : "request"}, {"name", name}, {"payload", payload}};
        if (!id.empty()) envelope["id"] = id;
        bridge.HandleWebMessage(target, envelope.dump());
    };
    const auto pump = [&](bool allow = true) { Legacy::Pump(bridge, {view, other}, allow); };
    // Publish() really uses float[22] market plus parallel floats/ints. Publish before any view.
    std::vector<float> market(22); market[0] = 1; market[8] = 250000; market[9] = 0.01f;
    setFloats(*vm, 0, {}, "x2357.ssse", "market", market);
    setFloats(*vm, 0, {}, "x2357.ssse", "prices", {100, 200.5f});
    setInts(*vm, 0, {}, "x2357.ssse", "stableIds", {3, 18});
    Legacy::Pump(bridge, {}, true);
    CHECK(wire.empty());
    bridge.OnViewCreated(view); bridge.OnViewCreated(other);
    send(view, "osfui.hello"); send(other, "osfui.hello");
    CHECK(std::ranges::count_if(wire, [&](const auto& m) { return m.first == view && m.second["kind"] == "state"; }) == 3);
    CHECK(std::ranges::none_of(wire, [&](const auto& m) { return m.first == other && m.second["kind"] == "state"; }));
    CHECK(providers.registrations == 0); // The adapter never invents a Settings schema.
    CHECK(providers.subscriptions == 1);
    wire.clear(); pump(); CHECK(wire.empty());
    bridge.OnViewCreated(view);
    setFloats(*vm, 0, {}, "x2357.ssse", "prices", {101, 201});
    send(view, "osfui.hello"); wire.clear(); pump(); CHECK(wire.empty()); // replay-before-drain, once
    for (int i = 0; i < 3; ++i) {
        Legacy::CloseView(bridge, view); bridge.OnViewCreated(view); send(view, "osfui.hello");
        CHECK(wire.size() == 4); wire.clear(); // ready + three retained values once
    }
    // Calls and argument order taken from installed main.js and PEX Arm/OnOSFUIViewAction.
    vm->calls.clear();
    send(view, "ui.action", {{"action", "buy"}, {"args", {3, 25}}});
    send(view, "ui.action", {{"action", "sell"}, {"args", {3, 4}}});
    send(view, "ui.action", {{"action", "buyOption"}, {"args", {3, 1, 100, 1200, 2, 17}}});
    CHECK(vm->calls.size() == 3);
    CHECK(vm->calls[0].handle == 123 && vm->calls[0].fn == "onosfuiviewaction");
    CHECK((vm->calls[0].parameterTypes == std::vector<std::string>{"string", "array"}));
    CHECK((vm->calls[0].args == std::vector<std::string>{"buy", "3", "25"}));
    CHECK((vm->calls[2].args == std::vector<std::string>{"buyOption", "3", "1", "100", "1200", "2", "17"}));
    send(other, "ui.action", {{"mod", "x2357.ssse"}, {"action", "buy"}, {"args", {3, 25}}});
    CHECK(vm->calls.size() == 3);
    send(view, "ui.action", {{"action", "refresh"}});
    CHECK((vm->calls.back().args == std::vector<std::string>{"refresh", ""})); // actual v1.6 fallback
    send(view, "ui.action", {{"action", "coerce"}, {"args", {1.25, true, nullptr, Json::object()}}});
    CHECK((vm->calls.back().args == std::vector<std::string>{"coerce", "1.250000", "true", "", ""}));

    const auto ask = [&](std::string name, Json args, std::string id = "q1", std::string target = "x2357.ssse/exchange") {
        send(target, "ui.papyrusRequest", {{"request", name}, {"args", args}, {"__osfuiV1TimeoutMs", 60000}}, id);
        return vm->calls.back().args.back();
    };
    const auto chart = ask("candles", {3, 30});
    CHECK(vm->calls.back().fn == "onosfuiviewrequest");
    CHECK((vm->calls.back().parameterTypes == std::vector<std::string>{"string", "array", "string"}));
    CHECK((vm->calls.back().args == std::vector<std::string>{"candles", "3", "30", chart}));
    CHECK(chart.starts_with("p"));
    // 30s modern timeout must not cut off a consumer's explicit 60s request.
    bridge.Tick(std::chrono::steady_clock::now() + std::chrono::seconds(31));
    CHECK(wire.empty());
    CHECK(replyFloats(*vm, 0, {}, chart, {95, 110, 90, 100}));
    CHECK(!replyFloats(*vm, 0, {}, chart, {999}));
    pump();
    CHECK(wire.back().first == view && wire.back().second["id"] == "q1");
    CHECK(wire.back().second["payload"]["type"] == "papyrus.result");
    CHECK(wire.back().second["payload"]["payload"]["value"] == Json({95, 110, 90, 100}));
    wire.clear();
    const auto quote = ask("optionQuote", {3, 1, 100, 1200}, "q2");
    CHECK(replyInt(*vm, 0, {}, quote, 17)); pump(); CHECK(wire.back().second["payload"]["payload"]["value"] == 17);
    const auto positions = ask("optionPositions", {15}, "q3");
    CHECK(replyInts(*vm, 0, {}, positions, {3, 1, 100, 1200, 2, 34, 17, 0})); pump();
    const auto bad = ask("unknown", {3}, "q4");
    CHECK(reject(*vm, 0, {}, bad, "bad-request", "unknown")); pump();
    CHECK(wire.back().second["payload"]["code"] == "bad-request");
    const auto timeout = ask("intraday", {3}, "q5");
    Legacy::Pump(bridge, {view}, true, std::chrono::steady_clock::now() + std::chrono::seconds(61));
    CHECK(wire.back().second["payload"]["code"] == "papyrus-timeout");
    CHECK(!replyFloats(*vm, 0, {}, timeout, {99}));
    const auto closing = ask("closes", {3, 122, 15}, "q1");
    Legacy::CloseView(bridge, view);
    CHECK(wire.back().second["payload"]["code"] == "view-closed");
    CHECK(!replyFloats(*vm, 0, {}, closing, {99}));
    bridge.OnViewCreated(view); send(view, "osfui.hello");
    const auto reopened = ask("intraday", {3}, "q1");
    CHECK(reopened != closing);
    CHECK(!replyFloats(*vm, 0, {}, closing, {99}));
    CHECK(replyFloats(*vm, 0, {}, reopened, {101})); pump();
    const auto removed = ask("intraday", {3}, "q6");
    CHECK(unregister(*vm, 0, {}, request)); CHECK(!unregister(*vm, 0, {}, request));
    CHECK(!replyFloats(*vm, 0, {}, removed, {99})); pump();
    CHECK(wire.back().second["payload"]["code"] == "unregistered");
    request = listenRequests(*vm, 0, {}, receiver, "x2357.ssse"); CHECK(request > 0);
    // The same page-local ID in another owning view has an independent reply token.
    bridge.OnViewCreated("x2357.ssse/chart"); send("x2357.ssse/chart", "osfui.hello");
    const auto firstView = ask("closes", {3, 7}, "shared");
    const auto secondView = ask("closes", {18, 7}, "shared", "x2357.ssse/chart");
    CHECK(firstView != secondView);
    CHECK(replyFloats(*vm, 0, {}, secondView, {200})); pump();
    CHECK(wire.back().first == "x2357.ssse/chart");
    CHECK(replyFloats(*vm, 0, {}, firstView, {100})); pump();
    CHECK(wire.back().first == view);
    const auto destroyed = ask("closes", {3, 7}, "reused");
    bridge.OnViewCreated(view); Legacy::CloseView(bridge, view); send(view, "osfui.hello");
    CHECK(!replyFloats(*vm, 0, {}, destroyed, {999}));
    // Bound in-flight requests independently of the modern bridge's larger cap.
    for (int i = 0; i < 32; ++i) ask("intraday", {3}, "limit" + std::to_string(i));
    const auto atCapacity = vm->calls.size(); ask("intraday", {3}, "overflow");
    CHECK(vm->calls.size() == atCapacity);
    CHECK(wire.back().second["payload"]["code"] == "papyrus-unavailable");
    Legacy::CloseView(bridge, view);

    // F7 is an observer of the canonical key, not a hard-coded shortcut.
    vm->calls.clear(); providers.Press(0x76); pump();
    CHECK(vm->calls.size() == 1 && vm->calls[0].fn == "onhotkey");
    CHECK((vm->calls[0].args == std::vector<std::string>{"x2357.ssse", "openExchange"}));
    API::BridgeApi::Get().SetViewCatalog({view});
    CHECK(open(*vm, 0, {}, view)); CHECK(API::BridgeApi::Get().TakePendingBatch().presentation.size() == 1);
    OSFSettings::API::HotkeyBlock block{}; settings.AcquireHotkeyBlock(&block);
    providers.Press(0x76); pump(); CHECK(vm->calls.size() == 1);
    settings.ReleaseHotkeyBlock(block);
    providers.Press(0x76, false); pump(); CHECK(vm->calls.size() == 1);
    providers.Press(0x76); pump(false); pump(); CHECK(vm->calls.size() == 1); // capture began after edge
    settings.key = 0x77; providers.Press(0x76); pump(); CHECK(vm->calls.size() == 1);
    providers.Press(0x77); pump(); CHECK(vm->calls.size() == 2);
    providers.Press(0x77); CHECK(unregister(*vm, 0, {}, hotkey)); pump(); CHECK(vm->calls.size() == 2);
    CHECK(providers.observers.empty());

    // Save-load lifecycle enters through the real modern SessionSink wired to the adapter.
    const auto loading = ask("candles", {3, 7}, "q7");
    RE::SaveLoadEvent::GetEventSource()->Notify({RE::SaveLoadEvent::OpType::kLoad, RE::SaveLoadEvent::Status::kBegin});
    const auto callsBeforeLoad = vm->calls.size();
    send(view, "ui.action", {{"action", "buy"}, {"args", {3, 25}}});
    CHECK(vm->calls.size() == callsBeforeLoad);
    CHECK(!replyFloats(*vm, 0, {}, loading, {99}));
    RE::SaveLoadEvent::GetEventSource()->Notify({RE::SaveLoadEvent::OpType::kLoad, RE::SaveLoadEvent::Status::kFailed});
    send(view, "ui.action", {{"action", "refresh"}}); CHECK(vm->calls.size() == callsBeforeLoad + 1);
    RE::TESLoadGameEvent::GetEventSource()->Notify({});
    CHECK(!unregister(*vm, 0, {}, action)); CHECK(!unregister(*vm, 0, {}, request));
    wire.clear(); pump();
    CHECK(wire[0].second["name"] == "data.reset");
    CHECK(!replyFloats(*vm, 0, {}, loading, {99}));
    wire.clear(); Legacy::ReplayState(bridge, view); CHECK(wire.empty());
    // The original Arm() pattern must work repeatedly after invalid saved tokens.
    const auto rearmed = listenActions(*vm, 0, {}, receiver, "x2357.ssse"); CHECK(rearmed != action && rearmed > 0);
    const auto rehotkey = registerHotkey(*vm, 0, {}, receiver, "OnHotkey", "x2357.ssse", "openExchange"); CHECK(rehotkey != hotkey);
    CHECK(listenRequests(*vm, 0, {}, receiver, "x2357.ssse") > 0);
    setFloats(*vm, 0, {}, "x2357.ssse", "market", market); pump();
    CHECK(providers.observers.size() == 1);
    CHECK(!unregister(*vm, 0, {}, hotkey)); // stale token cannot remove the new observer
    // A session reset before hello must not enqueue a reset after the fresh replay.
    bridge.OnViewCreated("x2357.ssse/chart");
    Legacy::ResetSession();
    setFloats(*vm, 0, {}, "x2357.ssse", "market", market);
    Legacy::Pump(bridge, {view, "x2357.ssse/chart"}, true);
    wire.clear(); send("x2357.ssse/chart", "osfui.hello");
    CHECK(wire.size() == 2 && wire.back().second["kind"] == "state");
    CHECK(listenActions(*vm, 0, {}, receiver, "x2357.ssse") > 0);

    // Malformed data cannot escape the dispatcher or mutate a different mod's state.
    wire.clear(); const auto beforeBad = vm->calls.size();
    send(view, "ui.action", {{"action", 10}});
    send(view, "ui.action", {{"action", std::string("bad\0name", 8)}});
    send("modern/view", "ui.action", {{"action", "buy"}, {"args", {3, 25}}});
    send(view, "ui.papyrusRequest", {{"request", Json::array()}}, "bad1");
    send(view, "ui.papyrusRequest", {{"request", "intraday"}, {"__osfuiV1TimeoutMs", -1}}, "bad2");
    send(view, "ui.papyrusRequest", {{"request", "intraday"}, {"args", std::vector<int>(129)}}, "bad3");
    CHECK(vm->calls.size() == beforeBad);
    CHECK(!replyInt(*vm, 0, {}, "123", 1)); CHECK(!replyInt(*vm, 0, {}, "p9999999", 1));
    wire.clear();
    setFloats(*vm, 0, {}, "x2357.ssse", "prices", {std::numeric_limits<float>::infinity()});
    setInts(*vm, 0, {}, "../bad", "market", {1});
    setInts(*vm, 0, {}, "x2357.ssse", "", {1}); pump(); CHECK(wire.empty());
    // Workers can publish while the runtime drains; latest value wins, without VM pointers.
    std::thread publisher([&] { for (int i = 0; i < 500; ++i) setInts(*vm, 0, {}, "x2357.ssse", "lastTrade", {i}); });
    for (int i = 0; i < 50; ++i) pump();
    publisher.join(); pump();
    CHECK(wire.back().second["value"] == Json({499}));
    // Replaced/dead VM receivers cannot keep a first-wins listener or a key observer alive.
    const auto dying = listenRequests(*vm, 0, {}, receiver, "x2357.ssse"); CHECK(dying > 0);
    vm->handlePolicy.unavailable.insert(123); pump();
    CHECK(!unregister(*vm, 0, {}, dying));
    vm->handlePolicy.unavailable.clear();
    CHECK(listenRequests(*vm, 0, {}, receiver, "x2357.ssse") > 0);
    API::Papyrus::OnMainMenuOpened(); pump(); CHECK(providers.observers.empty());
    CHECK(providers.registrations == 0);
    std::fprintf(stderr, "legacy_papyrus_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
