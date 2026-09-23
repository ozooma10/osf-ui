#pragma once

#include <mutex>
#include "OSFSettings.h"
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
        OSFSettings::API::Client _settings;
        OSFUI::API::Client _views;
        OSFSettings::API::Subscription _subscription{};
        std::mutex _publishMutex;
        bool _attempted{};
        bool _initialized{};
    };
}
