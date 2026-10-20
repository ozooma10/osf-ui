#include "Extension.h"
#include "World/ScreenPapyrus.h"
#include "REL/THook.h"

namespace
{
    using VM=RE::BSScript::IVirtualMachine;
    std::optional<REL::THook<void(VM**)>> bindHook;
    void Bind(VM** vm)
    {
        (*bindHook)(vm);
        if(vm && *vm) OSFUI::ScreenPapyrus::BindNatives(**vm);
    }
    void Message(SFSE::MessagingInterface::Message* message) noexcept
    {
        if(message && message->type==SFSE::MessagingInterface::kPostPostLoad) OSFUI::Extension::Connect();
    }
}
SFSE_PLUGIN_PRELOAD(const SFSE::PreLoadInterface* sfse)
{
    SFSE::Init(sfse,{.logLevel=REX::ELogLevel::Info,.logPattern="[%m-%d %T.%e] [%=5t] [%L] %v",.logRotate=1});
    return true;
}
SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* sfse)
{
    SFSE::Init(sfse,{.trampoline=true,.trampolineSize=256});
    bindHook.emplace("WorldScreens::Papyrus",RE::ID::GameVM::Ctor,0x802,&Bind);
    if(!bindHook->Init()) { REX::ERROR("World Screens Papyrus hook unavailable"); return false; }
    bindHook->Enable();
    const auto* messaging=SFSE::GetMessagingInterface();
    return messaging && messaging->RegisterListener(Message);
}
