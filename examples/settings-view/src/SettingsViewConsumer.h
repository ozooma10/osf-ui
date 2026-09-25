#pragma once

#include <mutex>
#include "vendor/OSFSettings.h"
#include "OSFUI.h"

namespace SettingsViewExample
{
    inline constexpr const char* kModId = "osfui-example";
    inline constexpr const char* kViewId = "osfui-example/panel";

    // The plugin owns one process-lifetime instance, matching Slim's callback lifetime.
    class Consumer final
    {
    public:
        bool Initialize();

    private:
        bool PublishSettings();
        static void OnSettingChanged(const char*, const char* key, void* user) noexcept;
        static void OnHotkey(const char*, const char*, void* user) noexcept;
        OSFSettings::API::Client m_settings;
        OSFUI::API::Client m_views;
        OSFSettings::API::Subscription m_subscription{};
        std::mutex m_publishMutex;
        bool m_attempted{};
        bool m_initialized{};
    };
}
