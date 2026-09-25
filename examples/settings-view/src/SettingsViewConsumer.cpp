#include "SettingsViewConsumer.h"

#include <REX/REX.h>
#include <string_view>

namespace SettingsViewExample
{
    bool Consumer::Initialize()
    {
        if (m_attempted) return m_initialized;
        m_attempted = true;
        if (!m_settings.Init() || !m_settings.IsReady() || !m_views.Init()) return false;
        // IsReady is intentionally false before the first WebView exists.
        if (!m_views.RegisterView(kViewId)) return false;
        const auto subscription = m_settings.Subscribe(kModId, &OnSettingChanged, this, &m_subscription);
        if (subscription != OSFSettings::API::Status::Ok) return false;
        if (!PublishSettings() || m_settings.RegisterHotkey(kModId, "openPanel", &OnHotkey, this) != OSFSettings::API::Status::Ok) {
            const auto status = m_settings.Unsubscribe(m_subscription);
            if (status != OSFSettings::API::Status::Ok) REX::ERROR("Example unsubscribe failed: {}", static_cast<unsigned>(status));
            return false;
        }
        m_initialized = true;
        return true;
    }

    bool Consumer::PublishSettings()
    {
        // Settings callbacks and the initial read can run on different threads.
        std::lock_guard lock(m_publishMutex);
        bool showDetails{};
        const auto status = m_settings.GetBool(kModId, "showDetails", &showDetails);
        if (status != OSFSettings::API::Status::Ok) {
            REX::WARN("Example setting read failed: {}", static_cast<unsigned>(status));
            return false;
        }
        // Explicit, minimal forwarding. No schema, binding data, or other mods' settings enter the page.
        return m_views.SetViewState(kModId, "showDetails", showDetails ? "true" : "false");
    }

    void Consumer::OnSettingChanged(const char*, const char* key, void* user) noexcept
    {
        if (key && std::string_view(key) != "showDetails") return;
        if (!static_cast<Consumer*>(user)->PublishSettings()) REX::WARN("Example state could not be published");
    }

    void Consumer::OnHotkey(const char*, const char*, void* user) noexcept
    {
        auto& self = *static_cast<Consumer*>(user);
        // SDK mutations enqueue work safely; this callback never calls the engine directly.
        if (self.PublishSettings() && !self.m_views.RequestMenu(kViewId, true)) {
            REX::WARN("Example view open could not be queued");
        }
    }
}
