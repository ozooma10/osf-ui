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
        // IsReady is false until the first WebView exists; registration does not need it.
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
        // Forward only this value; no schema or other mods' settings reach the page.
        return m_views.SetState(kModId, "showDetails", showDetails ? "true" : "false");
    }

    void Consumer::OnSettingChanged(const char*, const char* key, void* user) noexcept
    {
        if (key && std::string_view(key) != "showDetails") return;
        if (!static_cast<Consumer*>(user)->PublishSettings()) REX::WARN("Example state could not be published");
    }

    void Consumer::OnHotkey(const char*, const char*, void* user) noexcept
    {
        auto& self = *static_cast<Consumer*>(user);
        // SDK calls only enqueue work; never call the engine from this callback.
        if (self.PublishSettings() && !self.m_views.RequestMenu(kViewId, true)) {
            REX::WARN("Example view open could not be queued");
        }
    }
}
