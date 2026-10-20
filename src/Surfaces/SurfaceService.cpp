#include "Surfaces/SurfaceService.h"
#include "API/BridgeApi.h"
#include "Core/Paths.h"
#include "Core/Ids.h"
#include "Render/BrowserBuild.h"
#include "BrowserSurface.h"
#define NOGDI
#include "BrowserMessages.h"

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
    void SurfaceService::Initialize(MessageBridge& bridge, bool developer, std::function<std::optional<std::string>()> language,
        std::function<void(std::string_view)> resetPage, std::function<bool()> browserAllowed)
    {
        m_bridge = &bridge; m_developerMode = developer; m_language = std::move(language);
        m_resetPage = std::move(resetPage); m_browserAllowed = std::move(browserAllowed);
        for (const auto* action : {"navigate","back","forward","reload","stop","home","mute","clearData","close","getState"})
            bridge.RegisterRequest(std::string("browser.") + action,
                [this, action](const nlohmann::json& payload, MessageBridge& b) { BrowserRequest(action, payload, b); });
    }
    nlohmann::json SurfaceService::BrowserState(const Entry& e) const
    {
        auto state = e.browserState;
        state.erase("type"); state.erase("view");
        state["enabled"] = e.browserEnabled; state["fullscreen"] = e.browserFullscreen;
        const auto url = Json::Get(state,"url","");
        state["landing"] = !e.browserEnabled || url.empty() || url == "about:blank" || !e.browserError.empty();
        state["error"] = e.browserError;
        return state;
    }
    void SurfaceService::BrowserLayout(Entry& e)
    {
        if (e.manifest.browserChrome.empty() || !e.host) return;
        e.host->SetBrowserLayout({.chrome=e.manifest.id,
            .content=e.browserEnabled ? std::string(osfui::browser::kBrowserContent) : "",
            .chromeHeight=e.manifest.browserChromeHeight, .fullscreen=e.browserFullscreen,
            .landing=BrowserState(e)["landing"].get<bool>()});
        m_bridge->Emit(e.manifest.id,"browser.state",BrowserState(e));
    }
    void SurfaceService::RejectBrowserRequests(Entry& e)
    {
        for (auto token : e.browserRequests) m_bridge->RejectTo(token,"browser-unavailable","The browser stopped");
        e.browserRequests.clear();
    }
    void SurfaceService::BrowserRequest(std::string_view action, const nlohmann::json& payload, MessageBridge& b)
    {
        const auto found = std::ranges::find_if(m_entries, [&](const auto& e) {
            return e->manifest.id == b.CurrentSource() && e->manifest.id == osfui::browser::kBrowserView &&
                !e->manifest.browserChrome.empty() && e->host && !e->failed;
        });
        if (found == m_entries.end()) { b.Reject("browser-forbidden","Browser controls belong to the trusted browser chrome"); return; }
        auto& e = **found;
        if (action == "getState") { b.Respond(BrowserState(e)); return; }
        if (action == "close") {
            b.Respond(nlohmann::json::object());
            if (InputView() == e.manifest.id) FinishInteraction("closed");
            return;
        }
        if (!e.browserEnabled) { b.Reject("browsing-disabled","Web browsing is disabled"); return; }
        osfui::browser::msg::BrowserCommand command{.view=std::string(osfui::browser::kBrowserContent), .action=std::string(action)};
        if (action == "navigate" || action == "home") {
            const auto input = Json::Get(payload,"input","");
            if (action == "home" && input.empty()) command.url = "about:blank";
            else {
                const auto destination = osfui::browser::BrowserDestination(input, Json::Get(payload,"searchEngine","duckduckgo"));
                if (!destination) { b.Reject("invalid-url","Enter a public HTTP or HTTPS address, or search terms"); return; }
                command.url = *destination;
            }
            command.action = "navigate"; e.browserError.clear();
        } else if (action == "mute") {
            const auto muted = payload.find("muted");
            if (muted == payload.end() || !muted->is_boolean()) { b.Reject("invalid-payload","muted must be a boolean"); return; }
            command.value = muted->get<bool>();
        }
        std::erase_if(e.browserRequests, [&](auto token) { return !b.HasPending(token); });
        command.token = b.Defer();
        if (!e.host->SendBrowserCommand(command)) b.RejectTo(command.token,"browser-unavailable","The browser is not ready");
        else e.browserRequests.insert(command.token);
    }
    void SurfaceService::RequestBack() { m_backRequested = true; }
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
        if (m_resetPage) m_resetPage(entry.manifest.id);
        m_bridge->OnViewCreated(entry.manifest.id);
        API::BridgeApi::Get().SetViewInstantiated(entry.manifest.id, true);
        entry.host->CreateOrNavigateView(entry.manifest);
        entry.host->SetViewHidden(entry.manifest.id, false);
        if (!entry.manifest.browserChrome.empty()) {
            entry.browserFullscreen = false;
            entry.browserError.clear();
            entry.browserState = osfui::browser::msg::ToJson(osfui::browser::msg::BrowserState{
                .view=std::string(osfui::browser::kBrowserContent),.url="about:blank"});
            if (entry.browserEnabled) {
                entry.host->CreateOrNavigateRemoteView(osfui::browser::kBrowserContent,"about:blank",entry.manifest.height);
                entry.host->SetViewHidden(osfui::browser::kBrowserContent,false);
                entry.host->SetViewOrder(osfui::browser::kBrowserContent,-1);
            }
            BrowserLayout(entry);
        }
        entry.host->SetPointerInputEnabled(false);
        entry.host->Frames()->SetVisible(true);
    }
    void SurfaceService::Start(const std::shared_ptr<Entry>& entry)
    {
        if (entry->manifest.debugOnly && !m_developerMode) return;
        auto host = std::make_shared<BrowserHostClient>();
        auto profile = StringUtil::ToLowerAscii(entry->manifest.id); std::ranges::replace(profile, '/', '!');
        entry->browserEnabled = !entry->manifest.browserChrome.empty() && m_browserAllowed && m_browserAllowed();
        if (!host->Initialize({.width=entry->manifest.width, .height=entry->manifest.height, .devMode=m_developerMode,
            .backend=kCompiledBrowserBackend, .resolveLanguage=m_language, .dataDir=Paths::DataDir(), .instanceName="surface-"+profile,
            .remoteMode=entry->browserEnabled})) {
            entry->failed = true; return;
        }
        entry->host = std::move(host);
        { std::scoped_lock lock(m_mutex); entry->gpu = std::make_shared<SurfaceFrames>(entry->host->Frames()); }
        entry->host->SetWebMessageHandler([this, weak=std::weak_ptr(entry)](std::string_view id, std::string_view json) {
            if (const auto e=weak.lock(); e && !e->failed && id == e->manifest.id) m_bridge->HandleWebMessage(id, json);
        });
        entry->host->SetLoadHandler([this, weak=std::weak_ptr(entry)](const BrowserHostClient::LoadEvent& event) {
            if (const auto e=weak.lock()) {
                if (e->browserEnabled && event.viewId == osfui::browser::kBrowserContent) {
                    e->browserError = event.failed ? std::string(event.description) : "";
                    BrowserLayout(*e); return; // Site errors do not retire the chrome or end interaction.
                }
                if (event.viewId != e->manifest.id || (!event.failed && !e->recovery.IsAvailable() && !e->recovery.CanAcceptResponse())) return;
                e->loaded = !event.failed; e->failed = event.failed;
                if (event.failed) { (void)e->recovery.OnFailure(Now(), true); if (InputView() == e->manifest.id) CancelInteraction(); }
                else { e->recovery.OnResponse(Now()); }
            }
        });
        entry->host->SetBrowserStateHandler([this, weak=std::weak_ptr(entry)](const auto& state) {
            if (const auto e=weak.lock(); e && e->browserEnabled && state.view == osfui::browser::kBrowserContent) {
                e->browserState=osfui::browser::msg::ToJson(state);
                if (state.loading) e->browserError.clear();
                BrowserLayout(*e);
            }
        });
        entry->host->SetBrowserResultHandler([this, weak=std::weak_ptr(entry)](const auto& result) {
            if (const auto e=weak.lock(); e && e->browserRequests.erase(result.token)) {
                if (result.success) m_bridge->RespondTo(result.token,{{"restartRequired",result.restartRequired}});
                else m_bridge->RejectTo(result.token,"browser-command-failed",result.error);
            }
        });
        entry->host->SetBackHandler([this, weak=std::weak_ptr(entry)](std::string_view view, std::uint64_t epoch) {
            if (const auto e=weak.lock(); e && InputView() == e->manifest.id && view == e->manifest.id &&
                e->host->IsCurrentInputPresentation(view,epoch)) FinishInteraction("closed");
        });
        entry->host->SetCursorChangeHandler([this, weak=std::weak_ptr(entry->host)](std::uint32_t id) {
            if (FullscreenActive() && m_cursor && weak.lock() == m_input.load()) m_cursor(id);
        });
        entry->host->SetFailureHandler([this, weak=std::weak_ptr(entry)](const BrowserHostClient::FailureEvent& event) {
            if (const auto e=weak.lock()) {
                RejectBrowserRequests(*e);
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
                RejectBrowserRequests(*e);
                if (m_resetPage) m_resetPage(e->manifest.id);
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
    { return BeginInteractionWith(lease, {}, callback, context); }
    API::Surfaces::Interaction SurfaceService::BeginInteractionWith(API::Surfaces::Handle lease, const API::Surfaces::InteractionOptions& options, API::Surfaces::InteractionCallback callback, void* context) noexcept
    {
        std::scoped_lock lock(m_mutex);
        if (!callback || options.size < sizeof(API::Surfaces::InteractionOptions) || !m_leases.contains(lease) || m_requests.size() >= 64 || m_next == UINT64_MAX) return 0;
        if (options.presentation != API::Surfaces::Presentation::InWorld && options.presentation != API::Surfaces::Presentation::Fullscreen) return 0;
        const auto token=m_next++;
        m_requests.push_back({token,lease,callback,context,std::chrono::steady_clock::now(), false, m_leases.at(lease)->manifest.id,m_cancelGeneration.load(),options.presentation});
        return token;
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
    std::optional<std::pair<std::uint32_t, std::uint32_t>> SurfaceService::FullscreenSize() const
    {
        const auto packed=m_fullscreenSize.load();
        if (!packed) return std::nullopt;
        return std::pair{static_cast<std::uint32_t>(packed >> 32), static_cast<std::uint32_t>(packed)};
    }
    std::shared_ptr<SurfaceFrames> SurfaceService::FullscreenFrames() const
    { std::scoped_lock lock(m_mutex); return m_fullscreenFrames; }
    void SurfaceService::FinishInteraction(const char* reason)
    {
        auto previous=m_input.exchange(nullptr);
        if (previous) previous->SetInputFocus(false);
        if (m_fullscreenSize.exchange(0) && previous) previous->SetPointerInputEnabled(false);
        { std::scoped_lock lock(m_mutex); m_fullscreenFrames.reset(); }
        m_interactionToken=0; m_neutral=0;
        if (auto request=std::exchange(m_interaction, {})) {
            for (const auto& e : m_entries) if (e->manifest.id == request->view && e->browserFullscreen) {
                e->browserFullscreen=false; BrowserLayout(*e);
            }
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
    void SurfaceService::UpdateInteraction(bool allowed, bool neutral, bool fullscreenAvailable, const std::function<bool()>& admit)
    {
        if (m_backRequested.exchange(false) && m_interaction && m_interaction->started) {
            const auto e=Find(m_interaction->lease);
            if (e && !e->manifest.browserChrome.empty()) e->host->RequestBack(e->manifest.id);
            else CancelInteraction();
        }
        // Cancellation applies only to requests that existed at that event.
        // Loading/focus changes while idle cannot poison a later activation.
        const auto cancelled=[this](const Request& r) { return r.cancelGeneration != m_cancelGeneration.load(); };
        if (const auto ended=m_endRequested.exchange(0); ended && m_interaction && m_interaction->token == ended) FinishInteraction("ended");
        if (!allowed || (m_interaction && cancelled(*m_interaction))) FinishInteraction("focus-or-menu");
        std::deque<Request> requests;
        { std::scoped_lock lock(m_mutex); requests.swap(m_requests); }
        for (const auto& r : requests) {
            const auto e=Find(r.lease);
            const bool fullscreen=r.presentation == API::Surfaces::Presentation::Fullscreen;
            if (cancelled(r) || !allowed || m_interaction || !r.lease || !e) r.callback(r.token,API::Surfaces::Phase::Rejected,"unavailable",r.context);
            else if (fullscreen && !e->manifest.fullscreen) r.callback(r.token,API::Surfaces::Phase::Rejected,"fullscreen-unsupported",r.context);
            else if (fullscreen && !fullscreenAvailable) r.callback(r.token,API::Surfaces::Phase::Rejected,"fullscreen-unavailable",r.context);
            else { m_interaction=r; m_interactionToken=r.token; m_neutral=0; }
        }
        if (!m_interaction) return;
        const auto e=Find(m_interaction->lease);
        if (!e || e->failed || (!m_interaction->started && std::chrono::steady_clock::now()-m_interaction->requested > std::chrono::seconds(5))) {
            FinishInteraction("surface-unavailable"); return;
        }
        if (m_interaction->started) {
            // Browser layout changes invalidate the old presentation while the
            // same documents repaint. That temporary gap must not close controls.
            if (!e->loaded || !e->gpu || (e->manifest.browserChrome.empty() && !e->gpu->Ready()))
                FinishInteraction("surface-unavailable");
            return;
        }
        if (!e->loaded || !e->host || !e->gpu || !e->gpu->Ready()) return;
        if (!neutral) { m_neutral=0; return; }
        if (++m_neutral < 2) return;
        if (!admit()) { FinishInteraction("input-unavailable"); return; }
        e->host->SetInputTargetView(e->manifest.id);
        if (m_interaction->presentation == API::Surfaces::Presentation::Fullscreen) {
            if (!e->manifest.browserChrome.empty()) { e->browserFullscreen=true; BrowserLayout(*e); }
            e->host->SetPointerInputEnabled(true);
            { std::scoped_lock lock(m_mutex); m_fullscreenFrames=e->gpu; }
            m_fullscreenSize=(static_cast<std::uint64_t>(e->manifest.width) << 32) | e->manifest.height;
        }
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
