#include "Extension.h"
#include "OSFUI_WorldScreens.h"
#include "World/WorldTexture.h"
#include "World/ScreenBindingRuntime.h"
#include "World/ScreenBindings.h"
#include "World/ScreenAudio.h"
#include "World/ScreenInteraction.h"
#include "Core/NativeMainThreadQueue.h"
#include "REX/FModule.h"

namespace OSFUI::Extension
{
    namespace
    {
        API::Surfaces::IService* core{};
        std::vector<WorldTexture::SurfaceDesc> views;
        std::atomic_bool refreshPending{};
        struct Interaction
        {
            std::uint64_t token{}, coreToken{};
            std::uint32_t binding{};
            std::string view;
            API::WorldScreens::Callback callback{};
            void* context{};
            API::Surfaces::InteractionOptions options;
            std::chrono::steady_clock::time_point requested;
            bool cancelled{};
        };
        std::mutex interactionMutex;
        std::unordered_map<std::uint64_t,std::shared_ptr<Interaction>> interactions;
        std::uint64_t nextInteraction{1};
        void OnInteraction(std::uint64_t, API::Surfaces::Phase phase,const char* reason,void* context) noexcept
        {
            const auto token=static_cast<Interaction*>(context)->token;
            std::shared_ptr<Interaction> entry;
            { std::scoped_lock lock(interactionMutex);
              auto it=interactions.find(token); if (it==interactions.end()) return;
              entry=it->second; if (phase!=API::Surfaces::Phase::Started) interactions.erase(it); }
            entry->callback(token,phase,reason,entry->context);
        }
        void Tick(void*,bool dataLoaded) noexcept
        {
            try {
                                if (!WorldTexture::IsInstalled() && !WorldTexture::Install()) return;
                if (!dataLoaded) return;
                if (!refreshPending.exchange(true)) {
                    const auto result=NativeMainThreadQueue::Post([] {
                        struct ResetPending { ~ResetPending(){ refreshPending=false; } } resetPending;
                        static bool dataInitialized{};
                        if (!dataInitialized) { ScreenBindingRuntime::OnDataLoaded(); dataInitialized=true; }
                        ScreenBindings::Tick();
                        static auto audioAt=std::chrono::steady_clock::time_point{};
                        const auto now=std::chrono::steady_clock::now();
                        if (now-audioAt>=std::chrono::milliseconds(50)) {
                            core->PublishAudioScene("osfui.browser/tv",ScreenAudio::Capture());
                            audioAt=now;
                        }
                    },
                        "world-screen material refresh",[]{refreshPending=false;});
                    if (result==NativeMainThreadQueue::PostResult::Unavailable) refreshPending=false;
                }
                const auto counts=ScreenBindings::CountBindings();
                std::vector<std::uint32_t> demand;
                demand.reserve(views.size());
                for (const auto& view:views) {
                    const auto it=counts.find(view.id);
                    const auto count=it==counts.end() ? 0u : it->second;
                    demand.push_back(count);
                    if (view.id=="osfui.browser/tv") core->PublishScreenCount(view.id.c_str(),count);
                }
                WorldTexture::SetDemand(demand);
                std::vector<std::shared_ptr<Interaction>> pending;
                { std::scoped_lock lock(interactionMutex); for (const auto& [_,entry]:interactions) pending.push_back(entry); }
                for (const auto& entry:pending) {
                    const auto selection=ScreenBindings::Resolve(entry->binding);
                    bool cancelled;
                    { std::scoped_lock lock(interactionMutex); cancelled=entry->cancelled; }
                    const auto action=ScreenInteraction::Decide(entry->coreToken!=0,cancelled,selection,entry->view,
                        std::chrono::steady_clock::now()-entry->requested>std::chrono::seconds(5));
                    if (action==ScreenInteraction::Action::End) { core->EndInteraction(entry->coreToken); continue; }
                    if (action==ScreenInteraction::Action::Keep || action==ScreenInteraction::Action::Wait) continue;
                    if (action==ScreenInteraction::Action::Reject) {
                        OnInteraction(0,API::Surfaces::Phase::Rejected,"binding-unavailable",entry.get()); continue;
                    }
                    const auto view=std::ranges::find_if(views,[&](const auto& v){return v.id==entry->view;});
                    if (view==views.end()) continue;
                    const auto feed=WorldTexture::Feed(static_cast<std::size_t>(view-views.begin()));
                    if (!feed) continue;
                    entry->coreToken=core->BeginInteractionWith(feed,entry->options,OnInteraction,entry.get());
                    if (!entry->coreToken) OnInteraction(0,API::Surfaces::Phase::Rejected,"core-unavailable",entry.get());
                }
            } catch (const std::exception& error) { REX::ERROR("World Screens tick failed: {}",error.what()); }
        }
        class Service final : public API::WorldScreens::IScreens
        {
            bool IsReady() noexcept override { return core && WorldTexture::IsInstalled() && ScreenBindingRuntime::Enabled(); }
            std::uint32_t Bind(std::uint32_t ref,const char* view) noexcept override { return IsReady() && view ? ScreenBindings::Bind(ref,view) : 0; }
            bool Unbind(std::uint32_t token) noexcept override { return ScreenBindings::Unbind(token); }
            bool IsBound(std::uint32_t token) noexcept override { return ScreenBindings::Resolve(token).has_value(); }
            std::uint64_t BeginInteraction(std::uint32_t binding,API::WorldScreens::Callback callback,void* context) noexcept override
            { return BeginInteractionWith(binding,{},callback,context); }
            std::uint64_t BeginInteractionWith(std::uint32_t binding,const API::WorldScreens::InteractionOptions& options,API::WorldScreens::Callback callback,void* context) noexcept override
            {
                const auto selection=ScreenBindings::Resolve(binding);
                if (!IsReady() || !callback || !selection || options.size<sizeof(API::WorldScreens::InteractionOptions)) return 0;
                std::scoped_lock lock(interactionMutex);
                if (interactions.size()>=64 || nextInteraction==UINT64_MAX) return 0;
                const auto token=nextInteraction++;
                auto entry=std::make_shared<Interaction>();
                entry->token=token; entry->binding=binding; entry->view=selection->view;
                entry->callback=callback; entry->context=context; entry->options={.presentation=options.presentation}; entry->requested=std::chrono::steady_clock::now();
                interactions.emplace(token,std::move(entry)); return token;
            }
            bool EndInteraction(std::uint64_t token) noexcept override
            {
                std::scoped_lock lock(interactionMutex);
                const auto it=interactions.find(token); if(it==interactions.end()) return false;
                it->second->cancelled=true; return true;
            }
        };
    }
    API::WorldScreens::IScreens& Screens() { static Service service; return service; }
    const std::filesystem::path& DataDir()
    {
        static const auto path=std::filesystem::path(REX::FModule::GetExecutingModule().GetFileName()).parent_path()/"Data"/"SFSE"/"Plugins"/"OSF"/"WorldScreens";
        return path;
    }
    bool Connect()
    {
        core=API::Surfaces::RequestInterface();
        if (!core) { REX::ERROR("World Screens requires OSF UI with surface service ABI 1.3; extension disabled"); return false; }
        std::vector<WorldTexture::SurfaceDesc> discovered;
        core->VisitViews([](const API::Surfaces::View& view,void* context) noexcept {
            static_cast<std::vector<WorldTexture::SurfaceDesc>*>(context)->push_back({view.id,view.texture,view.width,view.height});
        },&discovered);
        auto catalog=WorldTexture::SelectSupportedSurfaces(discovered);
        for (const auto& rejected:catalog.rejected)
            REX::ERROR("World Screens: skipping view '{}': {}",rejected.id,rejected.reason);
        views=std::move(catalog.accepted);
        WorldTexture::SetService(core);
        if (views.empty() || !WorldTexture::Configure(views)) {
            REX::WARN("World Screens has no supported feeds or its rendering hook gate failed; extension disabled"); return false;
        }
        std::vector<std::string> ids; for(const auto& view:views) ids.push_back(view.id);
        ScreenBindingRuntime::Configure(std::move(ids)); ScreenBindingRuntime::Install();
        return core->RegisterTick(Tick,nullptr);
    }
}
extern "C" __declspec(dllexport) void* OSFUIWorldScreens_RequestAPI(std::uint32_t version,std::uint32_t* actual) noexcept
{
    if(actual) *actual=0;
    if(!OSFUI::API::Supports(OSFUI::API::WorldScreens::kVersion,version)) return nullptr;
    if(actual) *actual=OSFUI::API::WorldScreens::kVersion;
    return &OSFUI::Extension::Screens();
}
