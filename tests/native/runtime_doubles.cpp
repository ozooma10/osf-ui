#include "RuntimeDoubles.h"

#include "Composite/D3D12Compositor.h"
#include "Composite/D3D12Prologue.h"
#include "Composite/EngineD3D12.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Compat/V1/LegacyBridge.h"
#include "Compat/V1/LegacyViews.h"
#include "Compat/V1/PapyrusAdapter.h"
#include "Input/ControlLayer.h"
#include "Input/FocusMenu.h"
#include "Input/FreeCursor.h"
#include "Input/HardwareCursor.h"
#include "Input/MenuEventSink.h"
#include "Input/OverlayInputHook.h"
#include "Input/SimPause.h"
#include "Input/XInputPoller.h"
#include "Render/SharedFrameConsumer.h"
#include "Views/Dev/DevViewReloadWorker.h"
#include "BrowserMessages.h"
#include "check.h"

namespace RuntimeTest
{
    Environment env;

    void QueueLoad(std::string view)
    {
        env.notifications.push_back([view = std::move(view)] {
            env.hosts.at(env.viewHosts.at(view)).load({ .viewId = view, .url = "https://local/index.html" });
        });
    }
    void QueueHello(std::string view)
    {
        env.notifications.push_back([view = std::move(view)] {
            env.hosts.at(env.viewHosts.at(view)).message(view, R"({"kind":"send","name":"osfui.hello","payload":{}})");
        });
    }
    void QueueFailure(std::string stage, std::uint32_t code, std::string host)
    {
        env.notifications.push_back([stage = std::move(stage), code, host = std::move(host)] {
            env.hosts.at(host).failure({ .stage = stage, .description = "EXPECTED TEST: injected host failure", .errorCode = code });
        });
    }
}

using RuntimeTest::env;

namespace OSFUI
{
    // All renderer methods use the real class declaration so interface drift fails compilation.
    struct BrowserHostClient::Impl
    {
        std::string name;
        std::atomic_bool stopped{};
        std::shared_ptr<SharedFrameConsumer> frames = std::make_shared<SharedFrameConsumer>();
    };
    BrowserHostClient::BrowserHostClient() : m_impl(std::make_unique<Impl>()) {}
    BrowserHostClient::~BrowserHostClient() { Shutdown(); ++env.rendererDestructions; }
    void BrowserHostClient::Shutdown() { if (!m_impl->stopped.exchange(true)) ++env.rendererShutdowns; }
    bool BrowserHostClient::Initialize(const BrowserHostConfig& config)
    {
        ++env.rendererInitializations;
        m_impl->name = config.instanceName;
        env.remoteMode[m_impl->name]=config.remoteMode;
        env.rendererUsesCef = config.backend == osfui::browser::Backend::Cef;
        CHECK(env.settings.reads > 0); // Settings must be acquired before renderer setup.
        return env.rendererInitializes;
    }
    void BrowserHostClient::PrepareViewFiles() { ++env.filePreparations; }
    bool BrowserHostClient::RestartPending() const { return env.restartPending[m_impl->name]; }
    void BrowserHostClient::RestartAfterFailure()
    {
        ++env.restarts;
        for (const auto& [view, host] : env.viewHosts)
            if (host == m_impl->name) env.hidden.erase(view);
        if (env.inputTarget.empty() || env.viewHosts.at(env.inputTarget) == m_impl->name) env.browserFocus = false;
        if (m_impl->name.empty()) env.preparedFrameReady = false;
    }
    void BrowserHostClient::CreateOrNavigateView(const ViewManifest& manifest)
    {
        env.navigations.push_back(manifest.id);
        env.viewHosts[manifest.id] = m_impl->name;
        env.hidden[manifest.id] = true;
    }
    bool BrowserHostClient::RefreshModFiles(std::string_view mod)
    {
        env.refreshedMods.emplace_back(m_impl->name,mod);
        if (env.duringRefreshMod) env.duringRefreshMod(m_impl->name);
        return env.refreshFailsHost.empty() || m_impl->name != env.refreshFailsHost;
    }
    bool BrowserHostClient::CreateOrNavigateRemoteView(std::string_view id, std::string_view, std::uint32_t)
    {
        CHECK(env.remoteMode[m_impl->name]);
        env.navigations.emplace_back(id); env.viewHosts[std::string(id)]=m_impl->name;
        return true;
    }
    void BrowserHostClient::SetBrowserLayout(osfui::browser::msg::BrowserLayout layout) { env.browserLayouts.push_back(std::move(layout)); }
    bool BrowserHostClient::SendBrowserAudio(const osfui::browser::msg::BrowserAudio& audio) { env.browserAudio.push_back(audio); return true; }
    bool BrowserHostClient::SendBrowserCommand(const osfui::browser::msg::BrowserCommand& command)
    { env.browserCommands.push_back(command); return true; }
    void BrowserHostClient::SetBrowserStateHandler(BrowserStateHandler handler) { env.hosts[m_impl->name].browserState=std::move(handler); }
    void BrowserHostClient::SetBrowserResultHandler(BrowserResultHandler handler) { env.hosts[m_impl->name].browserResult=std::move(handler); }
    void BrowserHostClient::SetInputTargetView(std::string_view id)
    {
        CHECK(!env.settings.blocks.empty()); // Suppression must precede input focus.
        env.inputTarget = id;
    }
    void BrowserHostClient::Resize(std::uint32_t, std::uint32_t) {}
    void BrowserHostClient::SetViewport(std::uint32_t, std::uint32_t) {}
    void BrowserHostClient::ResetPresentation() { env.preparedFrameReady = false; }
    void BrowserHostClient::SetPointerInputEnabled(bool enabled) { env.pointerInput = enabled; }
    void BrowserHostClient::DrainNotifications()
    {
        auto pending = std::exchange(env.notifications, {});
        for (const auto& notify : pending) notify();
    }
    void BrowserHostClient::Update() {}
    std::shared_ptr<SharedFrameConsumer> BrowserHostClient::Frames() const { return m_impl->frames; }
    void BrowserHostClient::SendMessageToWeb(std::string_view view, std::string_view json)
    {
        env.sent.push_back({ std::string(view), nlohmann::json::parse(json) });
    }
    void BrowserHostClient::SetWebMessageHandler(WebMessageHandler handler) { env.message = handler; env.hosts[m_impl->name].message = std::move(handler); }
    void BrowserHostClient::SetLoadHandler(LoadHandler handler) { env.load = handler; env.hosts[m_impl->name].load = std::move(handler); }
    void BrowserHostClient::SetFailureHandler(FailureHandler handler) { env.failure = handler; env.hosts[m_impl->name].failure = std::move(handler); }
    void BrowserHostClient::SetCursorChangeHandler(CursorChangeHandler) {}
    void BrowserHostClient::SetBackHandler(BackHandler handler) { env.hosts[m_impl->name].back=std::move(handler); }
    void BrowserHostClient::RequestBack(std::string_view) { env.backWindowActive.push_back(env.rendererWindowActive); }
    bool BrowserHostClient::IsCurrentInputPresentation(std::string_view, std::uint64_t) const { return true; }
    void BrowserHostClient::SetInputFocus(bool focused)
    {
        if (focused) CHECK(!env.settings.blocks.empty() && env.engineFocus);
        env.browserFocus = focused;
    }
    void BrowserHostClient::InjectKeyEvent(std::uint32_t, bool) {}
    void BrowserHostClient::InjectKeyboard(const osfui::browser::msg::Keyboard& key) { env.keys.push_back(key.vk); }
    void BrowserHostClient::InjectText(const osfui::browser::msg::TextInput&) {}
    void BrowserHostClient::SetWindowActive(bool active) { env.rendererWindowActive = active; }
    void BrowserHostClient::GameWindowChanged() {}
    void BrowserHostClient::InjectMouseMove(int, int) {}
    void BrowserHostClient::InjectMouseButton(int x, int y, int button, bool down) { env.mouse.push_back({x, y, button, down}); }
    void BrowserHostClient::InjectMouseWheel(int, int, int) {}
    void BrowserHostClient::InjectPhysicalMouseWheel(int, int, int) {}
    void BrowserHostClient::OpenDevTools(std::string_view) {}
    void BrowserHostClient::SetConsoleHandler(std::string_view, ConsoleHandler) {}
    void BrowserHostClient::SetViewHidden(std::string_view view, bool hidden, bool) { env.hidden[std::string(view)] = hidden; }
    void BrowserHostClient::SetViewOrder(std::string_view, int) {}
    void BrowserHostClient::DestroyView(std::string_view view) { env.hidden.erase(std::string(view)); }

    // Capture/GPU readiness is supplied by the test; no GPU completion is simulated.
    SharedFrameConsumer::~SharedFrameConsumer() = default;
    struct D3D12Compositor::Impl {};
    D3D12Compositor::D3D12Compositor(std::shared_ptr<SharedFrameConsumer>) : m_impl(std::make_unique<Impl>()) { ++env.compositorCreations; }
    D3D12Compositor::~D3D12Compositor() = default;
    void D3D12Compositor::Update() {}
    void D3D12Compositor::SetVisible(bool visible) { env.compositorVisible = visible; }
    void D3D12Compositor::SetFullscreenSource(std::shared_ptr<SurfaceFrames> source) { env.compositorFullscreen = source != nullptr; }
    bool D3D12Compositor::PreparedFrameReady(std::uint32_t, std::uint32_t) const { return env.preparedFrameReady; }

    bool MenuEventSink::Install() { ++env.menuEventInstalls; return env.menuEventsInstall; }
    bool OverlayInputHook::KeysNeutral() { ++env.keyNeutralChecks; return env.keysNeutral; }
    bool MenuEventSink::SurfaceBlocked() { return env.transition; }
    bool MenuEventSink::ChargenOpen() { return false; }
    bool MenuEventSink::TransitionOpen() { return env.transition; }
    bool FocusMenu::Register() { ++env.focusRegistrations; return env.focusMenuRegisters; }
    bool FocusMenu::IsRegistered() { return env.focusRegistrations > 0 && env.focusMenuRegisters; }
    bool FocusMenu::IsOpenInEngine() { return env.engineFocus; }
    void FocusMenu::Open() { ++env.focusOpenRequests; } // Engine admission is a separate test input.
    void FocusMenu::Close() { ++env.focusCloseRequests; }
    void FocusMenu::SetGamepadCapture(bool) {}
    void ControlLayer::Apply(bool engage) { env.controlsBlocked = engage; }
    void FreeCursor::Apply(bool desired) { env.cursorFree = desired; }
    void SimPause::Apply(bool desired) { env.paused = desired; }
    namespace OverlayInputHook
    {
        bool Install() { ++env.inputHookInstallations; return env.inputHookInstalls; }
        void RequestStateRefresh() {}
        bool IsGameWindowActive() { return env.gameWindowActive; }
        std::optional<ClientSize> GameWindowClientSize() { return std::nullopt; }
    }
    namespace HardwareCursor { void SetSystemCursorId(std::uint32_t) {} }
    XInputPoller::State XInputPoller::Poll(double) { return env.gamepad; }
    void XInputPoller::Reset() {}

    EngineD3D12 LocateEngineD3D12()
    {
        static ID3D12Device device;
        static ID3D12CommandQueue queue;
        return { &device, &queue };
    }

    // No file worker or legacy/VM services run in this suite. Their adapters have separate tests.
    DevViewReloadWorker::DevViewReloadWorker(std::filesystem::path, Refresh refresh) { env.refreshMod=std::move(refresh); }
    DevViewReloadWorker::~DevViewReloadWorker() = default;
    void DevViewReloadWorker::SetMods(std::vector<std::string> mods) { env.reloadMods=std::move(mods); }
    std::vector<std::string> DevViewReloadWorker::DrainCompleted() { return std::exchange(env.reloadCompleted,{}); }
    namespace Paths
    {
        void Initialize() {}
        const std::filesystem::path& DataDir() { return env.dataDir; }
        std::filesystem::path ViewsDir() { return env.dataDir / "views"; }
    }
    namespace Log
    {
        bool DebugEnabled() { return false; }
        void SetDebugLogging(bool) {}
        void WarnOnce(std::once_flag& flag, std::string_view message)
        {
            std::call_once(flag, [&] { REX::test::Log("WARN", std::string(message)); });
        }
    }
    namespace Compat::V1
    {
        void DiscoverViews(ViewManager&) {}
        void Initialize() {}
        void Pump() {}
        namespace Papyrus
        {
            void RegisterEndpoints(MessageBridge&, std::function<bool(std::string_view)>) {}
            void CloseView(MessageBridge&, std::string_view) {}
            void ReplayState(MessageBridge&, std::string_view) {}
            void Pump(MessageBridge&, const std::vector<std::string>&, bool, std::chrono::steady_clock::time_point) {}
        }
    }
    namespace API::Papyrus
    {
        void Install() { ++env.papyrusInstalls; }
        PendingBatch TakePendingBatch() { return std::exchange(env.papyrus, {}); }
        ViewEndpoint ResolveViewEndpoint(std::string_view) { return {}; }
        bool OnViewSend(std::string_view, std::string_view, const std::vector<Value>&, std::string_view) { return false; }
        StaticDispatchResult OnViewRequest(std::string_view, std::string_view, const std::vector<Value>&, std::string_view, std::uint64_t) { return StaticDispatchResult::kVmUnavailable; }
        StaticDispatchResult DispatchStaticFunction(std::string_view, std::string_view, const std::vector<StaticCallArg>&) { return StaticDispatchResult::kVmUnavailable; }
    }
}

std::int32_t ID3D12Device::GetDeviceRemovedReason() { return env.gameDeviceHealthy ? 0 : static_cast<std::int32_t>(0x887A0005u); }
void ID3D12Device::Release() { ++env.deviceReleases; }
void ID3D12CommandQueue::Release() { ++env.queueReleases; }
