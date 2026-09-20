#include <SFSE/SFSE.h>
#include "SettingsViewConsumer.h"

namespace
{
    SettingsViewExample::Consumer consumer;
    void OnMessage(SFSE::MessagingInterface::Message* message)
    {
        if (!message || message->type != SFSE::MessagingInterface::kPostPostLoad) return;
        if (!consumer.Initialize()) REX::ERROR("OSF UI Settings example could not initialize");
    }
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* sfse)
{
    SFSE::Init(sfse);
    const auto* messaging = SFSE::GetMessagingInterface();
    return messaging && messaging->RegisterListener(OnMessage);
}
