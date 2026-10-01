#include "Compat/V1/PapyrusAdapter.h"

#include "API/PapyrusNames.h"
#include "Bridge/MessageBridge.h"
#include "Core/Ids.h"
#include "Core/StringUtil.h"
#include "RE/B/BSScriptUtil.h"
#include "RE/F/FORM_ENUM_STRING.h"
#include "RE/RTTI.h"
#include "RE/T/TESForm.h"
#include "RE/T/TESFullName.h"
#include "vendor/OSFSettings_Providers.h"
#include "vendor/OSFSettingsRegistry.h"
#include <atomic>
#include <cmath>
#include <map>
#include <set>

namespace OSFUI::Compat::V1::Papyrus
{
    namespace
    {
        using Json = nlohmann::json;
        using Str = RE::BSFixedString;
        using Object = RE::BSTSmartPointer<RE::BSScript::Object>;
        using IVM = RE::BSScript::IVirtualMachine;
        using VM = RE::BSScript::Internal::VirtualMachine;
        using Clock = std::chrono::steady_clock;
        namespace Settings = OSFSettings::API;
        using StringUtil::ToLowerAscii;
        enum class Kind { Action, Request, Hotkey };
        struct Receiver
        {
            std::uint64_t handle{};
            std::string script;
            bool operator==(const Receiver&) const = default;
        };
        struct KeyObserver
        {
            std::string mod, key;
            Settings::Subscription token{};
            std::atomic_uint presses{};
        };
        struct Entry
        {
            Kind kind{};
            Receiver receiver;
            std::string fn, mod, key;
            bool arrayArgs{true};
            // Callbacks only increment an atomic; provider unsubscribe fences their lifetime.
            std::vector<std::shared_ptr<KeyObserver>> observers;
        };
        struct Value
        {
            Json json;
            std::optional<std::vector<std::uint32_t>> forms;
        };
        struct StateValue
        {
            Value value;
            bool dirty{true};
            // Replay can run between a VM publish and the next pump.
            std::set<std::string> delivered;
        };
        struct Request
        {
            std::uint64_t bridgeToken{};
            std::int32_t owner{};
            std::string view;
            Clock::time_point deadline;
            bool answered{}, rejected{};
            std::string code, message;
            Value value;
        };
        struct State
        {
            // Also serializes VM enqueue against Unregister/reset. VM dispatch only queues;
            // it never waits for Papyrus to execute. Provider callbacks never take this lock.
            std::recursive_mutex lock;
            std::map<std::int32_t, Entry> entries;
            std::vector<std::shared_ptr<KeyObserver>> retired;
            std::map<std::pair<std::string, std::string>, StateValue> values;
            std::map<std::string, Request> requests;
            std::uint32_t nextRegistration{1}; // never reuse, even across save loads
            bool suspended{};
            std::set<std::pair<std::string, std::string>> resetKeys;
            Clock::time_point nextKeyScan{};
            Settings::Client settings;
            Settings::Providers::Client providers;
        };
        State& S() { static auto* state = new State; return *state; }

        bool Text(std::string_view text, std::size_t limit, bool empty = false)
        {
            return (empty || !text.empty()) && text.size() <= limit && text.find('\0') == text.npos;
        }
        bool ValidValue(const Json& value)
        {
            if (value.is_number_float()) return std::isfinite(value.get<double>());
            if (value.is_string()) return Text(value.get_ref<const std::string&>(), 65536, true);
            if (value.is_array()) return value.size() <= 4096 && std::ranges::all_of(value, ValidValue);
            return value.is_null() || value.is_boolean() || value.is_number_integer();
        }
        std::optional<Receiver> Resolve(IVM& vm, const Object& object)
        {
            if (!object || !object->IsValid() || !object->type) return {};
            const auto handle = object->GetHandle();
            const auto& policy = vm.GetObjectHandlePolicy();
            if (!handle || handle == policy.EmptyHandle() || !policy.IsHandleObjectAvailable(handle)) return {};
            const auto* script = object->type->name.c_str();
            if (!script || !PapyrusNames::IsScriptName(script)) return {};
            Object bound;
            if (!vm.FindBoundObject(handle, script, false, bound, true) || bound.get() != object.get()) return {};
            return Receiver{handle, ToLowerAscii(script)};
        }
        std::optional<Receiver> Resolve(IVM& vm, const Str& script)
        {
            if (!PapyrusNames::IsScriptName(script.c_str()) || Ids::EqualsCaseInsensitiveAscii(script.c_str(), "OSFUI")) return {};
            RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> type;
            if (!vm.GetScriptObjectType(script, type) || !type) return {};
            return Receiver{0, ToLowerAscii(script.c_str())};
        }
        void Retire(Entry& entry)
        {
            for (auto& observer : entry.observers) S().retired.push_back(std::move(observer));
            entry.observers.clear();
        }
        void PruneDeadReceivers();
        std::int32_t Register(Kind kind, std::optional<Receiver> receiver, const Str& fn,
            const Str& mod, const Str& key = {}, bool arrayArgs = true)
        {
            const auto id = ToLowerAscii(mod.c_str());
            if (!receiver || !Ids::IsValidModId(id) || !PapyrusNames::IsIdentifier(fn.c_str()) || !Text(key.c_str(), 128, true)) return 0;
            std::lock_guard lock(S().lock);
            if (S().suspended) return 0;
            PruneDeadReceivers();
            for (const auto& [token, entry] : S().entries) {
                if (entry.kind != kind || entry.mod != id) continue;
                // The historical request contract is first-listener-wins, including repeat calls.
                if (kind == Kind::Request) return 0;
                if (entry.receiver == *receiver && entry.fn == ToLowerAscii(fn.c_str()) &&
                    entry.key == ToLowerAscii(key.c_str()) && entry.arrayArgs == arrayArgs) return token;
            }
            if (S().entries.size() >= 0xFFFF || S().nextRegistration > INT32_MAX) return 0;
            const auto token = static_cast<std::int32_t>(S().nextRegistration++);
            S().entries.emplace(token, Entry{kind, std::move(*receiver), ToLowerAscii(fn.c_str()), id,
                ToLowerAscii(key.c_str()), arrayArgs, {}});
            if (kind == Kind::Hotkey) S().nextKeyScan = {};
            return token;
        }
        bool Unregister(IVM&, std::uint32_t, std::monostate, std::int32_t token)
        {
            std::lock_guard lock(S().lock);
            const auto found = S().entries.find(token);
            if (found == S().entries.end()) return false;
            Retire(found->second);
            S().entries.erase(found);
            for (auto& [_, request] : S().requests) {
                if (request.owner != token) continue;
                request.answered = request.rejected = true;
                request.code = "unregistered";
                request.message = "the Papyrus request listener was removed";
            }
            return true;
        }
        void PruneDeadReceivers()
        {
            auto* vm = VM::GetSingleton();
            if (!vm || S().suspended) return;
            std::vector<std::int32_t> dead;
            for (const auto& [token, entry] : S().entries) {
                if (!entry.receiver.handle) continue;
                Object object;
                if (!vm->GetObjectHandlePolicy().IsHandleObjectAvailable(entry.receiver.handle) ||
                    !vm->FindBoundObject(entry.receiver.handle, entry.receiver.script.c_str(), false, object, true) ||
                    !object || !object->IsValid()) dead.push_back(token);
            }
            for (const auto token : dead) Unregister(*vm, 0, {}, token);
        }
        // Called with the registry lock. Capture portable strings, never receiver pointers.
        bool Dispatch(const Entry& entry, std::string_view first, const std::vector<std::string>& args,
            std::string_view replyToken = {})
        {
            auto* vm = VM::GetSingleton();
            if (!vm || S().suspended) return false;
            if (entry.receiver.handle && !vm->GetObjectHandlePolicy().IsHandleObjectAvailable(entry.receiver.handle)) return false;
            const auto make = [kind = entry.kind, array = entry.arrayArgs, first = std::string(first), args, token = std::string(replyToken)]
                (RE::BSScrapArray<RE::BSScript::Variable>& out) {
                out.resize(kind == Kind::Request ? 3 : 2);
                out[0] = Str(first.c_str());
                if (kind == Kind::Hotkey || !array) out[1] = Str(args.empty() ? "" : args.front().c_str());
                else {
                    std::vector<Str> values;
                    for (const auto& arg : args) values.emplace_back(arg.c_str());
                    RE::BSScript::PackVariable(out[1], values);
                }
                if (kind == Kind::Request) out[2] = Str(token.c_str());
                return true;
            };
            const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback{};
            if (entry.receiver.handle) return vm->DispatchMethodCall(entry.receiver.handle, Str(entry.receiver.script.c_str()), Str(entry.fn.c_str()), make, callback, 0);
            return vm->DispatchStaticCall(Str(entry.receiver.script.c_str()), Str(entry.fn.c_str()), make, callback, 0);
        }
        Value Forms(const std::vector<RE::TESForm*>& forms)
        {
            Value result{nullptr, std::vector<std::uint32_t>{}};
            for (const auto* form : forms) result.forms->push_back(form ? form->GetFormID() : 0);
            return result;
        }
        Json Serialize(Value& value)
        {
            if (!value.forms) return value.json;
            auto array = Json::array();
            for (const auto id : *value.forms) {
                const auto* form = id ? RE::TESForm::LookupByID(id) : nullptr;
                if (!form) { array.push_back(nullptr); continue; }
                std::string type = std::to_string(static_cast<unsigned>(form->GetFormType()));
                for (const auto& item : RE::FORM_ENUM_STRING::GetFormEnumString())
                    if (item.formType == form->GetFormType() && item.formString) { type = item.formString; break; }
                Json item{{"formId", id}, {"formType", type}};
                if (const auto* named = starfield_cast<const RE::TESFullName*>(form))
                    if (const auto* name = named->GetFullName(); name && *name) item["name"] = name;
                if (const auto* editor = form->GetFormEditorID(); editor && *editor) item["editorId"] = editor;
                array.push_back(std::move(item));
            }
            value.forms.reset();
            value.json = std::move(array);
            return value.json;
        }
        void Set(const Str& mod, const Str& key, Value value)
        {
            const auto id = ToLowerAscii(mod.c_str());
            if (!Ids::IsValidModId(id) || !Text(key.c_str(), 64) || !ValidValue(value.json) || (value.forms && value.forms->size() > 4096)) return;
            std::lock_guard lock(S().lock);
            if (S().suspended) return;
            const auto address = std::pair{id, ToLowerAscii(key.c_str())};
            if (!S().values.contains(address) && S().values.size() >= 1024) return;
            S().values[address] = {std::move(value), true, {}};
        }
        bool Reply(const Str& token, Value value, std::string code = {}, std::string message = {}, bool rejected = false)
        {
            if (!ValidValue(value.json) || (value.forms && value.forms->size() > 4096) || !Text(code, 128, true) || !Text(message, 4096, true)) return false;
            std::lock_guard lock(S().lock);
            const auto found = S().requests.find(ToLowerAscii(token.c_str()));
            if (S().suspended || found == S().requests.end() || found->second.answered || Clock::now() >= found->second.deadline) return false;
            auto& request = found->second;
            request.answered = true;
            request.rejected = rejected;
            request.code = code.empty() && rejected ? "papyrus-rejected" : std::move(code);
            request.message = std::move(message);
            request.value = std::move(value);
            return true;
        }
        Json Strings(const std::vector<Str>& values)
        {
            auto json = Json::array();
            for (const auto& value : values) json.push_back(value.c_str());
            return json;
        }
        // The frozen signatures have void setters and bool replies (not modern Var).
#define LEGACY_VALUE(Name, Type, Encode) \
        void SetView##Name(IVM&, std::uint32_t, std::monostate, Str mod, Str key, Type value) { Set(mod, key, Encode); } \
        bool ReplyView##Name(IVM&, std::uint32_t, std::monostate, Str token, Type value) { return Reply(token, Encode); }
        LEGACY_VALUE(Bool, bool, (Value{value, {}}))
        LEGACY_VALUE(Int, std::int32_t, (Value{value, {}}))
        LEGACY_VALUE(Float, float, (Value{value, {}}))
        LEGACY_VALUE(String, Str, (Value{value.c_str(), {}}))
        LEGACY_VALUE(Bools, std::vector<bool>, (Value{Json(value), {}}))
        LEGACY_VALUE(Ints, std::vector<std::int32_t>, (Value{Json(value), {}}))
        LEGACY_VALUE(Floats, std::vector<float>, (Value{Json(value), {}}))
        LEGACY_VALUE(Strings, std::vector<Str>, (Value{Strings(value), {}}))
        LEGACY_VALUE(Forms, std::vector<RE::TESForm*>, Forms(value))
#undef LEGACY_VALUE
        bool RejectViewRequest(IVM&, std::uint32_t, std::monostate, Str token, Str code, Str message)
        { return Reply(token, {}, code.c_str(), message.c_str(), true); }
#define LEGACY_LISTENER(Name, KindName, Callback) \
        std::int32_t Name(IVM& vm, std::uint32_t, std::monostate, Object receiver, Str mod) \
        { return Register(Kind::KindName, Resolve(vm, receiver), Str(Callback), mod); } \
        std::int32_t Name##Static(IVM& vm, std::uint32_t, std::monostate, Str script, Str mod) \
        { return Register(Kind::KindName, Resolve(vm, script), Str(Callback), mod); }
        LEGACY_LISTENER(ListenForViewActions, Action, "OnOSFUIViewAction")
        LEGACY_LISTENER(ListenForViewRequests, Request, "OnOSFUIViewRequest")
#undef LEGACY_LISTENER
#define LEGACY_ACTION(Name, Array) \
        std::int32_t Name(IVM& vm, std::uint32_t, std::monostate, Object receiver, Str fn, Str mod) \
        { return Register(Kind::Action, Resolve(vm, receiver), fn, mod, {}, Array); } \
        std::int32_t Name##Static(IVM& vm, std::uint32_t, std::monostate, Str script, Str fn, Str mod) \
        { return Register(Kind::Action, Resolve(vm, script), fn, mod, {}, Array); }
        LEGACY_ACTION(RegisterForViewActions, false)
        LEGACY_ACTION(RegisterForViewActionsArgs, true)
#undef LEGACY_ACTION
        std::int32_t RegisterForHotkey(IVM& vm, std::uint32_t, std::monostate, Object receiver, Str fn, Str mod, Str key)
        { return Register(Kind::Hotkey, Resolve(vm, receiver), fn, mod, key); }
        std::int32_t RegisterForHotkeyStatic(IVM& vm, std::uint32_t, std::monostate, Str script, Str fn, Str mod, Str key)
        { return Register(Kind::Hotkey, Resolve(vm, script), fn, mod, key); }

        // v1.6 coercion, including scalar-action fallback and empty strings for complex values.
        std::optional<std::vector<std::string>> Args(const Json& payload, bool action)
        {
            std::vector<std::string> args;
            const auto values = payload.find("args");
            if (values != payload.end() && values->is_array()) {
                if (values->size() > 128) return {};
                for (const auto& value : *values) {
                    std::string text;
                    if (value.is_string()) text = value.get<std::string>();
                    else if (value.is_number_unsigned()) text = std::to_string(value.get<std::uint64_t>());
                    else if (value.is_number_integer()) text = std::to_string(value.get<std::int64_t>());
                    else if (value.is_number_float()) {
                        if (!std::isfinite(value.get<double>())) return {};
                        text = std::to_string(value.get<double>());
                    } else if (value.is_boolean()) text = value.get<bool>() ? "true" : "false";
                    if (!Text(text, 65536, true)) return {};
                    args.push_back(std::move(text));
                }
            } else if (action) {
                const auto scalar = payload.find("arg");
                const std::string text = scalar != payload.end() && scalar->is_string() ? scalar->get<std::string>() : "";
                if (!Text(text, 65536, true)) return {};
                args.push_back(text);
            }
            return args;
        }
        void ReconcileKeys(Clock::time_point now)
        {
            if (!S().settings && !S().settings.Init()) return;
            if (!S().providers && !S().providers.Init()) return;
            std::erase_if(S().retired, [](const auto& observer) {
                if (!observer->token) return true;
                const auto result = S().providers.UnsubscribeKey(observer->token);
                return result == Settings::Status::Ok || result == Settings::Status::UnknownSubscription;
            });
            if (now < S().nextKeyScan || std::ranges::none_of(S().entries, [](const auto& item) { return item.second.kind == Kind::Hotkey; })) return;
            S().nextKeyScan = now + std::chrono::seconds(1);
            std::vector<std::pair<std::string, std::string>> keys;
            const auto result = S().settings.ReadRegistry(nullptr, [](const Settings::RegistryView& registry, void* context) noexcept {
                auto& selected = *static_cast<decltype(keys)*>(context);
                for (std::uint32_t m = 0; m < registry.modCount; ++m) {
                    const auto& mod = registry.mods[m];
                    for (std::uint32_t g = 0; g < mod.groupCount; ++g)
                        for (std::uint32_t k = 0; k < mod.groups[g].settingCount; ++k) {
                            const auto& setting = mod.groups[g].settings[k];
                            if (setting.type == Settings::SettingType::Key)
                                selected.emplace_back(std::string(mod.id.data, mod.id.size), std::string(setting.key.data, setting.key.size));
                        }
                }
            }, &keys);
            if (result != Settings::Status::Ok) return;
            for (auto& [_, entry] : S().entries) {
                if (entry.kind != Kind::Hotkey) continue;
                // Schema replacement can remove a key. Stop observing it without creating
                // a replacement definition; OSF Settings remains the authority.
                std::erase_if(entry.observers, [&](const auto& observer) {
                    if (std::ranges::find(keys, std::pair{observer->mod, observer->key}) != keys.end()) return false;
                    S().retired.push_back(observer);
                    return true;
                });
                for (const auto& [mod, key] : keys) {
                    if (!Ids::EqualsCaseInsensitiveAscii(entry.mod, mod) || (!entry.key.empty() && !Ids::EqualsCaseInsensitiveAscii(entry.key, key))) continue;
                    if (std::ranges::any_of(entry.observers, [&](const auto& observer) { return observer->mod == mod && observer->key == key; })) continue;
                    auto observer = std::make_shared<KeyObserver>();
                    observer->mod = mod; observer->key = key;
                    if (S().providers.SubscribeKey(mod.c_str(), key.c_str(), [](const char*, const char*, void* context) noexcept {
                        auto& count = static_cast<KeyObserver*>(context)->presses;
                        auto n = count.load();
                        while (n < 32 && !count.compare_exchange_weak(n, n + 1)) {}
                    }, observer.get(), &observer->token) == Settings::Status::Ok) entry.observers.push_back(std::move(observer));
                }
            }
        }
    }

    void BindNatives(IVM& vm)
    {
#define BIND(Name) vm.BindNativeMethod("OSFUI", #Name, &Name, true, false)
#define BIND_VALUE(Name) BIND(SetView##Name); BIND(ReplyView##Name)
        BIND_VALUE(Bool); BIND_VALUE(Int); BIND_VALUE(Float); BIND_VALUE(String);
        BIND_VALUE(Bools); BIND_VALUE(Ints); BIND_VALUE(Floats); BIND_VALUE(Strings); BIND_VALUE(Forms);
#undef BIND_VALUE
        BIND(ListenForViewActions); BIND(ListenForViewActionsStatic);
        BIND(ListenForViewRequests); BIND(ListenForViewRequestsStatic);
        BIND(RegisterForViewActions); BIND(RegisterForViewActionsStatic);
        BIND(RegisterForViewActionsArgs); BIND(RegisterForViewActionsArgsStatic);
        BIND(RegisterForHotkey); BIND(RegisterForHotkeyStatic);
        BIND(RejectViewRequest); BIND(Unregister);
#undef BIND
    }
    void ResetSession()
    {
        std::lock_guard lock(S().lock);
        for (auto& [_, entry] : S().entries) Retire(entry);
        S().entries.clear();
        for (const auto& [address, _] : S().values) S().resetKeys.insert(address);
        S().values.clear();
        // Keep cancellation records until the runtime can retire bridge tokens.
        for (auto& [_, request] : S().requests) {
            request.answered = request.rejected = true;
            request.code = "game-load";
            request.message = "the request was canceled by a session change";
        }
    }
    void SetSuspended(bool suspended)
    {
        std::lock_guard lock(S().lock);
        S().suspended = suspended;
        for (auto& [_, entry] : S().entries)
            for (auto& observer : entry.observers) observer->presses.store(0);
    }
    void RegisterEndpoints(MessageBridge& bridge, std::function<bool(std::string_view)> admits)
    {
        bridge.RegisterSend("ui.action", [admits](const Json& payload, MessageBridge& b) {
            const std::string view(b.CurrentSource());
            const auto name = payload.find("action");
            auto args = Args(payload, true);
            if (!admits(view) || name == payload.end() || !name->is_string() || !Text(name->get_ref<const std::string&>(), 64) || !args) {
                b.ReportProtocolFault(view, "invalid-action", "invalid legacy action or source view"); return;
            }
            std::lock_guard lock(S().lock);
            for (const auto& [_, entry] : S().entries)
                if (entry.kind == Kind::Action && Ids::EqualsCaseInsensitiveAscii(entry.mod, Ids::ModOf(view)))
                    Dispatch(entry, name->get_ref<const std::string&>(), *args);
        });
        bridge.RegisterRequest("ui.papyrusRequest", [admits](const Json& payload, MessageBridge& b) {
            const std::string view(b.CurrentSource());
            const auto name = payload.find("request");
            auto args = Args(payload, false);
            if (!admits(view) || name == payload.end() || !name->is_string() || !Text(name->get_ref<const std::string&>(), 64) || !args) {
                b.Reject("invalid-request", "invalid legacy request or source view"); return;
            }
            // The v1.6 default remains ten seconds. The adapted helper carries an explicit
            // page timeout privately, bounded at 60s for Stock Exchange's slow chart walks.
            std::int64_t timeout = 10000;
            if (const auto ms = payload.find("__osfuiV1TimeoutMs"); ms != payload.end()) {
                if (!ms->is_number_integer() || *ms < 1 || *ms > 60000) { b.Reject("invalid-request", "invalid legacy request timeout"); return; }
                timeout = ms->get<std::int64_t>();
            }
            std::lock_guard lock(S().lock);
            if (S().suspended) { b.Reject("papyrus-unavailable", "Papyrus is loading a save"); return; }
            if (S().requests.size() >= 256 || std::ranges::count_if(S().requests, [&](const auto& r) { return r.second.view == view; }) >= 32) {
                b.Reject("papyrus-unavailable", "too many Papyrus requests in flight"); return;
            }
            for (const auto& [owner, entry] : S().entries) {
                if (entry.kind != Kind::Request || !Ids::EqualsCaseInsensitiveAscii(entry.mod, Ids::ModOf(view))) continue;
                const auto token = b.Defer(std::chrono::milliseconds(timeout + 1000));
                if (!token) return;
                const auto key = "p" + std::to_string(token);
                Request request;
                request.bridgeToken = token; request.owner = owner; request.view = view;
                request.deadline = Clock::now() + std::chrono::milliseconds(timeout);
                S().requests.emplace(key, std::move(request));
                if (!Dispatch(entry, name->get_ref<const std::string&>(), *args, key)) {
                    S().requests.erase(key);
                    b.RejectTo(token, "papyrus-unavailable", "the Papyrus listener is unavailable");
                }
                return;
            }
            b.Reject("papyrus-unavailable", "no Papyrus request listener is available");
        });
    }
    void Pump(MessageBridge& bridge, const std::vector<std::string>& views, bool allowHotkeys, Clock::time_point now)
    {
        std::lock_guard lock(S().lock);
        PruneDeadReceivers();
        ReconcileKeys(now);
        for (const auto& [_, entry] : S().entries) {
            for (const auto& observer : entry.observers) {
                const auto count = observer->presses.exchange(0);
                if (allowHotkeys && !S().suspended)
                    for (unsigned i = 0; i < count; ++i) Dispatch(entry, observer->mod, {observer->key});
            }
        }
        if (!S().resetKeys.empty()) {
            for (const auto& view : views) {
                // A pre-hello page has no old cache. Queuing a reset there would instead
                // erase its new-session replay when the event gate opens after hello.
                if (!bridge.IsGreeted(view)) continue;
                auto keys = Json::array();
                for (const auto& [mod, key] : S().resetKeys)
                    if (Ids::EqualsCaseInsensitiveAscii(mod, Ids::ModOf(view))) keys.push_back(key);
                if (!keys.empty()) bridge.Emit(view, "data.reset", {{"keys", keys}});
            }
            S().resetKeys.clear();
        }
        if (!S().suspended) {
            for (auto& [address, state] : S().values) {
                if (!state.dirty) continue;
                const auto value = Serialize(state.value);
                for (const auto& view : views) {
                    if (!Ids::EqualsCaseInsensitiveAscii(Ids::ModOf(view), address.first) || !bridge.IsGreeted(view) || state.delivered.contains(view)) continue;
                    bridge.PublishState(view, address.first, address.second, value);
                    state.delivered.insert(view);
                }
                state.dirty = false;
            }
        }
        for (auto it = S().requests.begin(); it != S().requests.end();) {
            auto& request = it->second;
            if (!bridge.HasPending(request.bridgeToken)) { it = S().requests.erase(it); continue; }
            if (request.answered && !S().suspended) {
                if (request.rejected) bridge.RejectTo(request.bridgeToken, request.code, request.message);
                else bridge.RespondTo(request.bridgeToken, Json{{"__osfuiV1Reply", true}, {"type", "papyrus.result"}, {"payload", {{"value", Serialize(request.value)}}}});
            } else if (now >= request.deadline) bridge.RejectTo(request.bridgeToken, "papyrus-timeout", "Papyrus did not answer the view request");
            else { ++it; continue; }
            it = S().requests.erase(it);
        }
    }
    void ReplayState(MessageBridge& bridge, std::string_view view)
    {
        std::lock_guard lock(S().lock);
        if (S().suspended) return;
        for (auto& [address, state] : S().values)
            if (Ids::EqualsCaseInsensitiveAscii(address.first, Ids::ModOf(view))) {
                bridge.PublishState(view, address.first, address.second, Serialize(state.value));
                state.delivered.insert(std::string(view));
            }
    }
    void CloseView(MessageBridge& bridge, std::string_view view)
    {
        std::lock_guard lock(S().lock);
        for (auto it = S().requests.begin(); it != S().requests.end();) {
            if (it->second.view != view) { ++it; continue; }
            if (bridge.HasPending(it->second.bridgeToken))
                bridge.RejectTo(it->second.bridgeToken, "view-closed", "the requesting view was closed");
            it = S().requests.erase(it);
        }
    }
}
