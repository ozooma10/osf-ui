// Optional OSF UI - World Screens extension. Acquire after SFSE kPostPostLoad.
#pragma once
#include "OSFUI_Surfaces.h"

namespace OSFUI::API::WorldScreens
{
    inline constexpr std::uint32_t kVersion = 0x00010001;
    using Binding = std::uint32_t;
    using Interaction = Surfaces::Interaction;
    using Phase = Surfaces::Phase;
    using Callback = Surfaces::InteractionCallback;
    using Presentation = Surfaces::Presentation;
    using InteractionOptions = Surfaces::InteractionOptions;
    struct IScreens
    {
        virtual bool IsReady() noexcept = 0;
        // Supported, privately instantiated ACTI geometry only. Zero means not ready/unsupported.
        // Tokens belong to this process and game session. Save the selected view ID instead.
        virtual Binding Bind(std::uint32_t reference, const char* view) noexcept = 0;
        virtual bool Unbind(Binding) noexcept = 0;
        virtual bool IsBound(Binding) noexcept = 0;
        // Admission waits for this binding's material and a completed browser frame.
        // The content mod owns activation distance/combat policy and gameplay state.
        virtual Interaction BeginInteraction(Binding, Callback, void*) noexcept = 0;
        virtual bool EndInteraction(Interaction) noexcept = 0;
        // 1.1. Fullscreen also requires the view's manifest to set "fullscreen": true.
        virtual Interaction BeginInteractionWith(Binding, const InteractionOptions&, Callback, void*) noexcept = 0;
    protected:
        ~IScreens() = default;
    };
    inline IScreens* RequestInterface(std::uint32_t version = kVersion) noexcept
    {
        const auto module=REX::W32::GetModuleHandleW(L"OSFUIWorldScreens.dll");
        const auto request=module ? reinterpret_cast<AcquireFn>(REX::W32::GetProcAddress(module,"OSFUIWorldScreens_RequestAPI")) : nullptr;
        std::uint32_t actual{};
        auto* api=request ? request(version,&actual) : nullptr;
        return Supports(actual,version) ? static_cast<IScreens*>(api) : nullptr;
    }
    class Client
    {
    public:
        bool Init() noexcept { api=RequestInterface(); return api!=nullptr; }
        bool IsReady() const noexcept { return api && api->IsReady(); }
        Binding Bind(std::uint32_t ref,const char* view) const noexcept { return api ? api->Bind(ref,view) : 0; }
        bool Unbind(Binding token) const noexcept { return api && api->Unbind(token); }
        bool IsBound(Binding token) const noexcept { return api && api->IsBound(token); }
        Interaction BeginInteraction(Binding token,Callback fn,void* context) const noexcept { return api ? api->BeginInteraction(token,fn,context) : 0; }
        Interaction BeginInteraction(Binding token,const InteractionOptions& options,Callback fn,void* context) const noexcept { return api ? api->BeginInteractionWith(token,options,fn,context) : 0; }
        bool EndInteraction(Interaction token) const noexcept { return api && api->EndInteraction(token); }
    private:
        IScreens* api{};
    };
}
