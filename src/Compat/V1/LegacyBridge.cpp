#include "Compat/V1/LegacyBridge.h"
#include "Compat/V1/SettingsCodec.h"
#include "API/BridgeApi.h"
#include "Core/Json.h"
#include "vendor/OSFSettings_Providers.h"
#include "vendor/OSFSettingsRegistry.h"
#include "vendor/OSFSettings_Diagnostics.h"
#include "RE/U/UIMessageQueue.h"
#include <cstring>
#include <map>
#include <set>

namespace OSFUI::Compat::V1
{
    namespace
    {
        namespace Settings = OSFSettings::API;
        auto& Current() { return API::BridgeApi::Get(); }
        std::string Text(Settings::TextView value) { return {value.data ? value.data : "", value.size}; }
        void ReadValues(const Settings::RegistryView& registry, void* context) noexcept
        {
            auto& values = *static_cast<Document*>(context);
            for (std::uint32_t m = 0; m < registry.modCount; ++m)
                for (std::uint32_t g = 0; g < registry.mods[m].groupCount; ++g) {
                    const auto& group = registry.mods[m].groups[g];
                    for (std::uint32_t s = 0; s < group.settingCount; ++s) {
                        const auto& setting = group.settings[s];
                        const auto key = Text(setting.key);
                        switch (setting.type) {
                        case Settings::SettingType::Bool: values[key] = setting.value.boolean; break;
                        case Settings::SettingType::Int: values[key] = setting.value.integer; break;
                        case Settings::SettingType::Float: values[key] = setting.value.number; break;
                        case Settings::SettingType::Key: values[key] = KeyName(setting.value.key); break;
                        default: values[key] = Text(setting.value.text); break;
                        }
                    }
                }
        }

        class LegacyBridge final : public IOSFUIBridge
        {
            struct Provider { SettingsDefinition definition; Settings::Providers::Registration token{}; };
            struct Listener
            {
                std::string mod, key;
                SettingChangedFn changed{};
                HotkeyFn hotkey{};
                void* user{};
                Settings::Subscription token{};
                std::atomic_bool dirty{true};
                std::atomic_uint presses{};
                Document previous = Document::object();
                bool active{true};
            };
            struct Endpoint { CommandFn send{}; RequestFn request{}; void* user{}; };
            std::recursive_mutex mutex;
            Settings::Client settings;
            Settings::Providers::Client providers;
            Settings::Diagnostics::Client diagnostics;
            std::map<std::string, std::shared_ptr<Provider>> definitions;
            std::map<std::uint32_t, std::shared_ptr<Listener>> listeners;
            std::map<std::string, Endpoint> sends, requests;
            std::map<std::string, std::set<std::string>> issues;
            std::uint32_t nextToken{1};
            bool initialized{};

            bool Publish(Provider& provider)
            {
                auto& definition = provider.definition;
                const auto schema = definition.schema.dump();
                const auto values = definition.values.dump();
                const auto status = providers.Register(definition.id.c_str(), schema.c_str(), values.c_str(),
                    [](const char*, const char* json, void* context) noexcept {
                        auto parsed = Document::parse(json, nullptr, false);
                        return !parsed.is_discarded() && SaveValues(static_cast<Provider*>(context)->definition, parsed);
                    }, &provider, &provider.token);
                if (status != Settings::Status::Ok) {
                    REX::WARN("Legacy settings '{}' registration failed: {}", definition.id, static_cast<unsigned>(status));
                    return false;
                }
                REX::INFO("Legacy settings '{}' supplied through OSF Settings", definition.id);
                return true;
            }
            void Activate(Listener& listener)
            {
                if (listener.token || !initialized) return;
                const auto status = listener.changed ? settings.Subscribe(listener.mod.c_str(),
                    [](const char*, const char*, void* context) noexcept { static_cast<Listener*>(context)->dirty.store(true); }, &listener, &listener.token) :
                    providers.SubscribeKey(listener.mod.c_str(), listener.key.c_str(),
                    [](const char*, const char*, void* context) noexcept { static_cast<Listener*>(context)->presses.fetch_add(1); }, &listener, &listener.token);
                if (status != Settings::Status::Ok) REX::WARN("Legacy subscription '{}' failed: {}", listener.mod, static_cast<unsigned>(status));
            }
            void Remove(std::uint32_t token, bool hotkey)
            {
                std::lock_guard lock(mutex);
                const auto found = listeners.find(token);
                if (found == listeners.end() || bool(found->second->hotkey) != hotkey) return;
                const auto listener = found->second;
                listener->active = false;
                if (listener->token) {
                    if (hotkey) providers.UnsubscribeKey(listener->token);
                    else settings.Unsubscribe(listener->token);
                }
                listeners.erase(token);
            }
        public:
            void Initialize()
            {
                std::lock_guard lock(mutex);
                if (initialized) return;
                diagnostics.Init();
                providers.Init();
                if (!settings.Init() || !settings.IsReady() || !providers) {
                    REX::ERROR("OSF UI 1.6 compatibility requires OSF Settings with runtime provider support");
                    diagnostics.Report({"osfui", "compat.settings", Settings::Diagnostics::Severity::Error,
                        "Legacy mod settings are unavailable", "The installed OSF Settings lacks the runtime provider service.",
                        "Install the OSF Settings build supplied with this OSF UI update and restart Starfield."});
                    return;
                }
                diagnostics.Clear("osfui", "compat.settings");
                initialized = true;
                for (auto& [id, definition] : definitions) if (!definition->token) Publish(*definition);
                // Runtime registrations made during plugin loading take precedence over files.
                const auto directory = Root() / "settings";
                std::error_code ec;
                for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
                    if (!it->is_regular_file(ec) || it->path().extension() != ".json") continue;
                    const auto schema = Json::ParseFile(it->path());
                    if (!schema || !schema->is_object()) continue;
                    const auto id = Json::Get(*schema, "id", "");
                    if (id == "osfui" || definitions.contains(id)) continue;
                    RegisterSettingsSchema(schema->dump().c_str());
                }
                for (auto& [token, listener] : listeners) Activate(*listener);
            }
            void Pump()
            {
                std::lock_guard lock(mutex);
                if (!initialized) return;
                std::vector<std::shared_ptr<Listener>> pending;
                for (const auto& [token, listener] : listeners) pending.push_back(listener);
                for (const auto& listener : pending) {
                    if (!listener->active) continue;
                    if (listener->changed && listener->dirty.exchange(false)) {
                        auto values = Document::object();
                        if (settings.ReadRegistry(listener->mod.c_str(), &ReadValues, &values) != Settings::Status::Ok) continue;
                        auto previous = std::move(listener->previous);
                        listener->previous = values;
                        for (const auto& [key, value] : values.items()) {
                            if (!listener->active) break;
                            if (!previous.contains(key) || previous[key] != value) {
                                const auto json = value.dump();
                                listener->changed(listener->mod.c_str(), key.c_str(), json.c_str(), listener->user);
                            }
                        }
                    }
                    const auto presses = listener->presses.exchange(0);
                    for (unsigned i = 0; i < presses && listener->active; ++i)
                        listener->hotkey(listener->mod.c_str(), listener->key.c_str(), listener->user);
                }
            }
            std::uint32_t GetInterfaceVersion() override { return kBridgeVersion; }
            void GetPluginVersion(std::uint32_t& major, std::uint32_t& minor, std::uint32_t& patch) override { major = 2; minor = patch = 0; }
            const char* GetBridgeProtocolVersion() override { return "1.5"; }
            bool IsBridgeReady() override { return Current().IsReady(); }
            void RegisterCommand(const char* name, CommandFn fn, void* user) override
            {
                if (!name || !fn) return;
                std::lock_guard lock(mutex);
                auto [it, added] = sends.try_emplace(name);
                if (added && !Current().RegisterSend(name, [](const char* name, const char* json, const char* view, void* context) noexcept {
                    auto& self = *static_cast<LegacyBridge*>(context);
                    std::lock_guard lock(self.mutex);
                    const auto entry = self.sends.find(name);
                    if (entry != self.sends.end() && entry->second.send) {
                        const auto callback = entry->second;
                        callback.send(name, json, view, callback.user);
                    }
                }, this)) { sends.erase(it); return; }
                it->second = {fn, nullptr, user};
            }
            void UnregisterCommand(const char* name) override
            {
                std::lock_guard lock(mutex);
                if (name && sends.erase(name)) Current().RemoveOwnedEndpoint(name, this);
            }
            bool SendToWeb(const char* view, const char* type, const char* json) override { return Current().EmitEvent(view, type, json); }
            void SetReadyCallback(ReadyFn fn, void* user) override { Current().SetLegacyReadyCallback(fn, user); }
            bool RequestMenu(const char* view, bool open) override
            {
                if (view && std::string_view(view) == "osfui/settings") {
                    if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
                        queue->AddMessage(RE::BSFixedString("OSFSettingsMenu"), open ? RE::UI_MESSAGE_TYPE::kShow : RE::UI_MESSAGE_TYPE::kHide);
                        return true;
                    }
                    return false;
                }
                return Current().RequestMenu(view, open);
            }
            std::uint32_t SubscribeSettings(const char* mod, SettingChangedFn fn, void* user) override
            {
                if (!mod || !*mod || !fn) return 0;
                std::lock_guard lock(mutex);
                if (!nextToken) return 0;
                auto listener = std::make_shared<Listener>();
                listener->mod = mod; listener->changed = fn; listener->user = user;
                Activate(*listener);
                const auto token = nextToken++;
                listeners.emplace(token, std::move(listener));
                return token;
            }
            void UnsubscribeSettings(std::uint32_t token) override { Remove(token, false); }
            bool GetSettingBool(const char* mod, const char* key, bool* out) override { std::lock_guard lock(mutex); return settings.GetBool(mod, key, out) == Settings::Status::Ok; }
            bool GetSettingInt(const char* mod, const char* key, std::int64_t* out) override { std::lock_guard lock(mutex); return settings.GetInt(mod, key, out) == Settings::Status::Ok; }
            bool GetSettingFloat(const char* mod, const char* key, double* out) override { std::lock_guard lock(mutex); return settings.GetFloat(mod, key, out) == Settings::Status::Ok; }
            std::uint32_t GetSettingString(const char* mod, const char* key, char* out, std::uint32_t size) override
            {
                std::lock_guard lock(mutex);
                std::string value;
                std::uint32_t code{};
                if (settings.GetString(mod, key, value) != Settings::Status::Ok && settings.GetEnum(mod, key, value) != Settings::Status::Ok) {
                    if (settings.GetKey(mod, key, &code) != Settings::Status::Ok) return 0;
                    value = KeyName(code);
                }
                if (out && size) { const auto length = std::min<std::size_t>(value.size(), size - 1); std::memcpy(out, value.data(), length); out[length] = 0; }
                return static_cast<std::uint32_t>(value.size() + 1);
            }
            bool RegisterSettingsSchema(const char* json) override
            {
                if (!json) return false;
                auto source = Document::parse(json, nullptr, false);
                if (source.is_discarded() || !source.is_object()) return false;
                std::lock_guard lock(mutex);
                // Validate identity before using it as a path.
                std::string error;
                auto definition = TranslateSettings(source, Document::object(), error);
                if (!definition) { REX::WARN("Legacy settings schema rejected: {}", error); return false; }
                const auto path = Root() / "settings" / "values" / (definition->id + ".json");
                std::error_code ec;
                if (std::filesystem::exists(path, ec)) {
                    auto saved = Json::ParseFile(path);
                    if (!saved || !saved->is_object()) { REX::ERROR("Legacy values '{}' cannot be read; preserving file", definition->id); return false; }
                    definition = TranslateSettings(source, *saved, error);
                    if (!definition) return false;
                }
                auto provider = std::make_shared<Provider>();
                provider->definition = std::move(*definition);
                const auto id = provider->definition.id;
                if (definitions.contains(id)) provider->token = definitions.at(id)->token;
                if (initialized && !Publish(*provider)) return false;
                definitions[id] = std::move(provider);
                return true;
            }
            void UnregisterSettingsSchema(const char* mod) override
            {
                std::lock_guard lock(mutex);
                if (!mod) return;
                const auto found = definitions.find(mod);
                if (found == definitions.end()) return;
                if (found->second->token) providers.Unregister(found->second->token);
                definitions.erase(found);
            }
            std::uint32_t SubscribeHotkey(const char* mod, const char* key, HotkeyFn fn, void* user) override
            {
                if (!mod || !*mod || !key || !*key || !fn) return 0;
                std::lock_guard lock(mutex);
                if (!nextToken) return 0;
                auto listener = std::make_shared<Listener>();
                listener->mod = mod; listener->key = key; listener->hotkey = fn; listener->user = user;
                Activate(*listener);
                const auto token = nextToken++;
                listeners.emplace(token, std::move(listener));
                return token;
            }
            void UnsubscribeHotkey(std::uint32_t token) override { Remove(token, true); }
            bool RegisterView(const char* view) override { return Current().RegisterView(view); }
            bool ReportIssue(const char* mod, const char* id, const char* code, std::uint32_t severity, const char* subject, const char* context) override
            {
                std::lock_guard lock(mutex);
                const auto status = diagnostics.Report({mod, id, severity == 1 ? Settings::Diagnostics::Severity::Error : Settings::Diagnostics::Severity::Warning,
                    subject && *subject ? subject : code, context, "Update the owning mod or inspect its log for details."});
                if (status != Settings::Status::Ok) return false;
                issues[mod].insert(id);
                return true;
            }
            bool ClearIssue(const char* mod, const char* id) override
            {
                std::lock_guard lock(mutex);
                if (diagnostics.Clear(mod, id) != Settings::Status::Ok) return false;
                if (mod && id) issues[mod].erase(id);
                return true;
            }
            bool ClearIssuesExcept(const char* mod, const char* keepJson) override
            {
                if (!mod || !keepJson) return false;
                const auto keep = Document::parse(keepJson, nullptr, false);
                if (!keep.is_array()) return false;
                std::lock_guard lock(mutex);
                const auto owned = issues[mod];
                bool ok = true;
                for (const auto& id : owned) if (std::ranges::find(keep, Document(id)) == keep.end()) ok = ClearIssue(mod, id.c_str()) && ok;
                return ok;
            }
            void RegisterRequest(const char* name, RequestFn fn, void* user) override
            {
                if (!name || !fn) return;
                std::lock_guard lock(mutex);
                auto [it, added] = requests.try_emplace(name);
                if (added && !Current().RegisterRequest(name, [](const API::Request& current, void* context) noexcept {
                    auto& self = *static_cast<LegacyBridge*>(context);
                    std::lock_guard lock(self.mutex);
                    const auto found = self.requests.find(current.name);
                    if (found == self.requests.end() || !found->second.request) { current.Reject("unregistered", "legacy handler was removed"); return; }
                    const auto callback = found->second;
                    Request request{current.name, current.payloadJson, current.sourceViewId, current.m_token,
                        [](std::uint64_t token, const char* type, const char* json) noexcept {
                            const auto parsed = json ? Json::Parse(json) : std::nullopt;
                            if (!parsed) { Current().RejectRequest(token, "invalid-response", "legacy handler returned invalid JSON"); return; }
                            const auto reply = Json::Dump({{"__osfuiV1Reply", true}, {"type", type && *type ? type : "ui.result"}, {"payload", *parsed}});
                            Current().ReplyRequest(token, reply.c_str());
                        },
                        [](std::uint64_t token, const char* code, const char* message) noexcept { Current().RejectRequest(token, code, message); }};
                    callback.request(request, callback.user);
                }, this)) { requests.erase(it); return; }
                it->second = {nullptr, fn, user};
            }
            void UnregisterRequest(const char* name) override
            {
                std::lock_guard lock(mutex);
                if (name && requests.erase(name)) Current().RemoveOwnedEndpoint(name, this);
            }
            bool SetViewState(const char* mod, const char* key, const char* json) override
            {
                return Current().SetState(mod, key, json);
            }
        };
        LegacyBridge& Instance() { static auto* bridge = new LegacyBridge; return *bridge; }
    }
    IOSFUIBridge& Bridge() { return Instance(); }
    void Initialize() { Instance().Initialize(); }
    void Pump() { Instance().Pump(); }
}

extern "C" __declspec(dllexport) void* OSFUI_RequestBridge(std::uint32_t version) noexcept
{
    if ((version >> 16) != (OSFUI::Compat::V1::kBridgeVersion >> 16) ||
        (version & 0xFFFF) > (OSFUI::Compat::V1::kBridgeVersion & 0xFFFF)) return nullptr;
    return &OSFUI::Compat::V1::Bridge();
}
