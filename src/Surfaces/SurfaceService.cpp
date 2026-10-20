#include "Surfaces/SurfaceService.h"
#include "API/BridgeApi.h"
#include "Core/Paths.h"
#include "Core/Ids.h"
#include "Render/BrowserBuild.h"

namespace OSFUI
{
    namespace { double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); } }
    SurfaceService& SurfaceService::Get() { static auto* instance = new SurfaceService; return *instance; }
    void SurfaceService::Configure(const std::vector<ViewManifest>& views)
    {
        std::scoped_lock lock(m_mutex);
        if (!m_entries.empty()) return;
        for (const auto& view : views) if (view.kind == ViewKind::Surface) {
            auto entry = std::make_shared<Entry>(); entry->manifest = view; m_entries.push_back(std::move(entry));
        }
    }
    void SurfaceService::Initialize(MessageBridge& bridge, bool developer, std::function<std::optional<std::string>()> language)
    { m_bridge = &bridge; m_developerMode = developer; m_language = std::move(language); }
    void SurfaceService::VisitViews(API::Surfaces::Visitor visit, void* context) noexcept
    {
        if (!visit) return;
        std::vector<ViewManifest> views;
        { std::scoped_lock lock(m_mutex); for (const auto& e : m_entries) views.push_back(e->manifest); }
        for (const auto& v : views) visit({v.id.c_str(), v.texture.c_str(), v.width, v.height}, context);
    }
    bool SurfaceService::RegisterTick(API::Surfaces::Tick tick, void* context) noexcept
    {
        std::scoped_lock lock(m_mutex);
        if (!tick || (m_tick && (m_tick != tick || m_tickContext != context))) return false;
        m_tick = tick; m_tickContext = context; return true;
    }
    std::shared_ptr<SurfaceService::Entry> SurfaceService::Find(API::Surfaces::Handle handle) const
    {
        std::scoped_lock lock(m_mutex);
        const auto it = m_leases.find(handle); return it == m_leases.end() ? nullptr : it->second;
    }
    API::Surfaces::Handle SurfaceService::Acquire(const char* id) noexcept
    {
        if (!id) return 0;
        std::scoped_lock lock(m_mutex);
        if (m_leases.size() >= 4096 || m_next == UINT64_MAX) return 0;
        for (const auto& e : m_entries) if (Ids::EqualsCaseInsensitiveAscii(e->manifest.id, id)) {
            const auto token = m_next++; m_leases.emplace(token, e); ++e->demand; return token;
        }
        return 0;
    }
    void SurfaceService::Release(API::Surfaces::Handle handle) noexcept
    {
        std::scoped_lock lock(m_mutex);
        const auto it = m_leases.find(handle);
        if (it == m_leases.end()) return;
        --it->second->demand; m_leases.erase(it);
    }
    API::Surfaces::FrameStatus SurfaceService::CopyFrame(API::Surfaces::Handle handle, ID3D12Resource* output, std::uint64_t destinationGeneration, ID3D12CommandQueue* queue) noexcept
    {
        std::shared_ptr<SurfaceFrames> gpu;
        { std::scoped_lock lock(m_mutex); const auto it = m_leases.find(handle); if (it != m_leases.end()) gpu = it->second->gpu; }
        return gpu ? gpu->Copy(output, destinationGeneration, queue) : API::Surfaces::FrameStatus{};
    }
    bool SurfaceService::IsIdle(API::Surfaces::Handle handle) noexcept
    {
        std::shared_ptr<SurfaceFrames> gpu;
        { std::scoped_lock lock(m_mutex); const auto it = m_leases.find(handle); if (it != m_leases.end()) gpu = it->second->gpu; }
        return !gpu || gpu->Idle();
    }
    void SurfaceService::Navigate(Entry& entry)
    {
        entry.loaded = false;
        m_bridge->OnViewCreated(entry.manifest.id);
        API::BridgeApi::Get().SetViewInstantiated(entry.manifest.id, true);
        entry.host->CreateOrNavigateView(entry.manifest);
        entry.host->SetViewHidden(entry.manifest.id, false);
        entry.host->SetPointerInputEnabled(false);
        entry.host->Frames()->SetVisible(true);
    }
    void SurfaceService::Start(const std::shared_ptr<Entry>& entry)
    {
        if (entry->manifest.debugOnly && !m_developerMode) return;
        auto host = std::make_shared<BrowserHostClient>();
        auto profile = StringUtil::ToLowerAscii(entry->manifest.id); std::ranges::replace(profile, '/', '!');
        if (!host->Initialize({.width=entry->manifest.width, .height=entry->manifest.height, .devMode=m_developerMode,
            .backend=kCompiledBrowserBackend, .resolveLanguage=m_language, .dataDir=Paths::DataDir(), .instanceName="surface-"+profile})) {
            entry->failed = true; return;
        }
        entry->host = std::move(host);
        { std::scoped_lock lock(m_mutex); entry->gpu = std::make_shared<SurfaceFrames>(entry->host->Frames()); }
        entry->host->SetWebMessageHandler([this, weak=std::weak_ptr(entry)](std::string_view id, std::string_view json) {
            if (const auto e=weak.lock(); e && !e->failed && id == e->manifest.id) m_bridge->HandleWebMessage(id, json);
        });
        entry->host->SetLoadHandler([this, weak=std::weak_ptr(entry)](const BrowserHostClient::LoadEvent& event) {
            if (const auto e=weak.lock()) {
                if (event.viewId != e->manifest.id || (!event.failed && !e->recovery.IsAvailable() && !e->recovery.CanAcceptResponse())) return;
                e->loaded = !event.failed; e->failed = event.failed;
                if (event.failed) { (void)e->recovery.OnFailure(Now(), true); if (InputView() == e->manifest.id) CancelInteraction(); }
                else { e->recovery.OnResponse(Now()); }
            }
        });
        entry->host->SetFailureHandler([this, weak=std::weak_ptr(entry)](const BrowserHostClient::FailureEvent& event) {
            if (const auto e=weak.lock()) {
                e->loaded=false; e->failed=true;
                if (BrowserHostRecovery::IsRetryable(event.stage, event.errorCode)) (void)e->recovery.OnFailure(Now(), true);
                else (void)e->recovery.OnFailure(Now(), false);
                if (InputView() == e->manifest.id) CancelInteraction();
                REX::ERROR("Surface '{}' failed at {}: {}", e->manifest.id, event.stage, event.description);
            }
        });
        Navigate(*entry);
    }
    void SurfaceService::Update(bool dataLoaded)
    {
        API::Surfaces::Tick tick; void* context;
        { std::scoped_lock lock(m_mutex); tick=m_tick; context=m_tickContext; }
        if (tick) tick(context, dataLoaded);
        if (!m_bridge) return;
        for (const auto& e : m_entries) {
            if (e->retirement.valid()) {
                if (e->retirement.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
                e->retirement.get();
            }
            std::size_t demand;
            { std::scoped_lock lock(m_mutex); demand=e->demand; }
            if (!demand && e->host) {
                if (InputView() == e->manifest.id) FinishInteraction("surface-released");
                if (e->gpu && !e->gpu->Idle()) continue;
                m_bridge->OnViewDestroyed(e->manifest.id);
                API::BridgeApi::Get().SetViewInstantiated(e->manifest.id, false);
                { std::scoped_lock lock(m_mutex); e->gpu.reset(); }
                e->retirement=std::async(std::launch::async, [host=std::move(e->host)]() mutable { host.reset(); });
                e->loaded=false; e->failed=false; e->recovery.Reset();
                continue;
            }
            if (demand && !e->host && !e->failed) Start(e);
            if (!e->host) continue;
            e->host->DrainNotifications();
            (void)e->recovery.ExpireResponseWait(Now());
            e->recovery.ObserveHealth(Now());
            if (e->recovery.BeginDueAttempt(Now())) {
                e->failed=false; e->host->RestartAfterFailure(); Navigate(*e);
            }
            e->host->Update();
        }
    }
    bool SurfaceService::HasAvailableViews() const
    { return std::ranges::any_of(m_entries, [](const auto& e) { return e->host && !e->failed && e->loaded; }); }
    const char* SurfaceService::LoadState(std::string_view id) const
    {
        for (const auto& e : m_entries) if (e->manifest.id == id)
            return e->failed ? "failed" : (!e->host ? "unloaded" : (e->loaded ? "loaded" : "loading"));
        return "unloaded";
    }
    bool SurfaceService::IsInstantiated(std::string_view id) const
    { return std::ranges::any_of(m_entries, [&](const auto& e) { return e->host && e->manifest.id == id; }); }
    bool SurfaceService::Send(std::string_view id, std::string_view json)
    { for (const auto& e : m_entries) if (e->host && e->manifest.id == id) { e->host->SendMessageToWeb(id,json); return true; } return false; }
    API::Surfaces::Interaction SurfaceService::BeginInteraction(API::Surfaces::Handle lease, API::Surfaces::InteractionCallback callback, void* context) noexcept
    {
        std::scoped_lock lock(m_mutex);
        if (!callback || !m_leases.contains(lease) || m_requests.size() >= 64 || m_next == UINT64_MAX) return 0;
        const auto token=m_next++; m_requests.push_back({token,lease,callback,context,std::chrono::steady_clock::now(), false, m_leases.at(lease)->manifest.id,m_cancelGeneration.load()}); return token;
    }
    bool SurfaceService::EndInteraction(API::Surfaces::Interaction token) noexcept
    {
        if (!token) return false;
        // Ends only this interaction; CancelInteraction would also reject everyone else's queued requests.
        if (m_interactionToken.load() == token) { m_endRequested=token; return true; }
        std::scoped_lock lock(m_mutex);
        for (auto& r : m_requests) if (r.token == token) { r.lease=0; return true; }
        return false;
    }
    std::optional<std::string> SurfaceService::InputView() const
    {
        if (!m_interaction || !m_interaction->started) return std::nullopt;
        return m_interaction->view;
    }
    void SurfaceService::FinishInteraction(const char* reason)
    {
        auto previous=m_input.exchange(nullptr);
        if (previous) previous->SetInputFocus(false);
        m_interactionToken=0; m_neutral=0;
        if (auto request=std::exchange(m_interaction, {})) {
            if (m_bridge && request->started)
                m_bridge->Emit(request->view,"ui.interaction",nlohmann::json{{"active",false},{"reason",reason}});
            request->callback(request->token,request->started ? API::Surfaces::Phase::Ended : API::Surfaces::Phase::Rejected,reason,request->context);
        }
    }
    bool SurfaceService::WantsInteraction() const
    {
        if (m_interactionToken.load()) return true;
        std::scoped_lock lock(m_mutex); return !m_requests.empty();
    }
    void SurfaceService::UpdateInteraction(bool allowed, bool neutral, const std::function<bool()>& admit)
    {
        // Cancellation applies only to requests that existed at that event.
        // Loading/focus changes while idle cannot poison a later activation.
        const auto cancelled=[this](const Request& r) { return r.cancelGeneration != m_cancelGeneration.load(); };
        if (const auto ended=m_endRequested.exchange(0); ended && m_interaction && m_interaction->token == ended) FinishInteraction("ended");
        if (!allowed || (m_interaction && cancelled(*m_interaction))) FinishInteraction("focus-or-menu");
        std::deque<Request> requests;
        { std::scoped_lock lock(m_mutex); requests.swap(m_requests); }
        for (const auto& r : requests) {
            if (cancelled(r) || !allowed || m_interaction || !r.lease) r.callback(r.token,API::Surfaces::Phase::Rejected,"unavailable",r.context);
            else { m_interaction=r; m_interactionToken=r.token; m_neutral=0; }
        }
        if (!m_interaction) return;
        const auto e=Find(m_interaction->lease);
        if (!e || e->failed || (!m_interaction->started && std::chrono::steady_clock::now()-m_interaction->requested > std::chrono::seconds(5))) {
            FinishInteraction("surface-unavailable"); return;
        }
        if (m_interaction->started) {
            if (!e->loaded || !e->gpu || !e->gpu->Ready()) FinishInteraction("surface-unavailable");
            return;
        }
        if (!e->loaded || !e->host || !e->gpu || !e->gpu->Ready()) return;
        if (!neutral) { m_neutral=0; return; }
        if (++m_neutral < 2) return;
        if (!admit()) { FinishInteraction("input-unavailable"); return; }
        e->host->SetInputTargetView(e->manifest.id);
        m_input.store(e->host);
        m_interaction->started=true;
        m_bridge->Emit(e->manifest.id,"ui.interaction",nlohmann::json{{"active",true}});
        m_interaction->callback(m_interaction->token,API::Surfaces::Phase::Started,"requested",m_interaction->context);
    }
}

#if defined(_WIN32)
#define OSFUI_SURFACE_EXPORT __declspec(dllexport)
#else
#define OSFUI_SURFACE_EXPORT
#endif
extern "C" OSFUI_SURFACE_EXPORT void* OSFUI_RequestSurfaces(std::uint32_t version, std::uint32_t* actual) noexcept
{
    if (actual) *actual=0;
    if (!OSFUI::API::Supports(OSFUI::API::Surfaces::kVersion,version)) return nullptr;
    if (actual) *actual=OSFUI::API::Surfaces::kVersion;
    return static_cast<OSFUI::API::Surfaces::IService*>(&OSFUI::SurfaceService::Get());
}
