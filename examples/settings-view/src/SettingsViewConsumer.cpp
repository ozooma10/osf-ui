#include "SettingsViewConsumer.h"

#include <REX/REX.h>
#include <string_view>

namespace SettingsViewExample
{
    bool Consumer::Initialize()
    {
        if (_attempted) return _initialized;
        _attempted = true;
        if (!_settings.Init() || !_settings.IsReady() || !_views.Init()) return false;
        // IsReady is intentionally false before the first WebView exists.
        if (!_views.RegisterView(kViewId)) return false;
        const auto subscription = _settings.Subscribe(kModId, &OnSettingChanged, this, &_subscription);
        if (subscription != OSFSettings::API::Status::Ok) return false;
        if (!PublishSettings() || _settings.RegisterHotkey(kModId, "openPanel", &OnHotkey, this) != OSFSettings::API::Status::Ok) {
            const auto status = _settings.Unsubscribe(_subscription);
            if (status != OSFSettings::API::Status::Ok) REX::ERROR("Example unsubscribe failed: {}", static_cast<unsigned>(status));
            return false;
        }
        _initialized = true;
        return true;
    }

    bool Consumer::PublishSettings()
    {
        // Settings callbacks and the initial read can run on different threads.
        std::lock_guard lock(_publishMutex);
        bool showDetails{};
        const auto status = _settings.GetBool(kModId, "showDetails", &showDetails);
        if (status != OSFSettings::API::Status::Ok) {
            REX::WARN("Example setting read failed: {}", static_cast<unsigned>(status));
            return false;
        }
        // Explicit, minimal forwarding. No schema, binding data, or other mods' settings enter the page.
        return _views.SetViewState(kModId, "showDetails", showDetails ? "true" : "false");
    }

    void Consumer::OnSettingChanged(const char*, const char* key, void* user) noexcept
    {
        if (key && std::string_view(key) != "showDetails") return;
        try {
            if (!static_cast<Consumer*>(user)->PublishSettings()) REX::WARN("Example state could not be published");
        } catch (...) { REX::ERROR("Example settings callback failed"); }
    }

    void Consumer::OnHotkey(const char*, const char*, void* user) noexcept
    {
        try {
            auto& self = *static_cast<Consumer*>(user);
            // SDK mutations enqueue work safely; this callback never calls the engine directly.
            if (self.PublishSettings() && !self._views.RequestMenu(kViewId, true)) {
                REX::WARN("Example view open could not be queued");
            }
        } catch (...) { REX::ERROR("Example hotkey callback failed"); }
    }
}
