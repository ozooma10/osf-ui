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
#include "Wv2Messages.h"
#include "check.h"

namespace RuntimeTest
{
    Environment env;

    void QueueLoad(std::string view)
    {
        env.notifications.push_back([view = std::move(view)] {
            env.load({ .viewId = view, .url = "https://local/index.html" });
        });
    }
    void QueueHello(std::string view)
    {
        env.notifications.push_back([view = std::move(view)] {
            env.message(view, R"({"kind":"send","name":"osfui.hello","payload":{}})");
        });
    }
    void QueueFailure(std::string stage, std::uint32_t code)
    {
        env.notifications.push_back([stage = std::move(stage), code] {
            env.failure({ .stage = stage, .description = "EXPECTED TEST: injected host failure", .errorCode = code });
        });
    }
}

using RuntimeTest::env;

namespace OSFUI
{
    // All renderer methods use the real class declaration so interface drift fails compilation.
    struct WebView2HostWebRenderer::Impl
    {
        std::shared_ptr<SharedFrameConsumer> frames = std::make_shared<SharedFrameConsumer>();
    };
    WebView2HostWebRenderer::WebView2HostWebRenderer() : m_impl(std::make_unique<Impl>()) {}
    WebView2HostWebRenderer::~WebView2HostWebRenderer() = default;
    bool WebView2HostWebRenderer::Initialize(const WebView2HostConfig&)
    {
        ++env.rendererInitializations;
        CHECK(env.settings.reads > 0); // Settings must be acquired before renderer setup.
        return env.rendererInitializes;
    }
    void WebView2HostWebRenderer::PrepareViewFiles() { ++env.filePreparations; }
    void WebView2HostWebRenderer::RestartAfterFailure()
    {
        ++env.restarts;
        env.hidden.clear();
        env.browserFocus = false;
        env.preparedFrameReady = false;
    }
    void WebView2HostWebRenderer::CreateOrNavigateView(const ViewManifest& manifest)
    {
        env.navigations.push_back(manifest.id);
        env.hidden[manifest.id] = true;
    }
    bool WebView2HostWebRenderer::RefreshModFiles(std::string_view) { return true; }
    void WebView2HostWebRenderer::SetInputTargetView(std::string_view id)
    {
        CHECK(!env.settings.blocks.empty()); // Suppression must precede input focus.
        env.inputTarget = id;
    }
    void WebView2HostWebRenderer::Resize(std::uint32_t, std::uint32_t) {}
    void WebView2HostWebRenderer::SetViewport(std::uint32_t, std::uint32_t) {}
    void WebView2HostWebRenderer::ResetPresentation() { env.preparedFrameReady = false; }
    void WebView2HostWebRenderer::SetPointerInputEnabled(bool) {}
    void WebView2HostWebRenderer::DrainNotifications()
    {
        auto pending = std::exchange(env.notifications, {});
        for (const auto& notify : pending) notify();
    }
    void WebView2HostWebRenderer::Update() {}
    std::shared_ptr<SharedFrameConsumer> WebView2HostWebRenderer::Frames() const { return m_impl->frames; }
    void WebView2HostWebRenderer::SendMessageToWeb(std::string_view view, std::string_view json)
    {
        env.sent.push_back({ std::string(view), nlohmann::json::parse(json) });
    }
    void WebView2HostWebRenderer::SetWebMessageHandler(WebMessageHandler handler) { env.message = std::move(handler); }
    void WebView2HostWebRenderer::SetLoadHandler(LoadHandler handler) { env.load = std::move(handler); }
    void WebView2HostWebRenderer::SetFailureHandler(FailureHandler handler) { env.failure = std::move(handler); }
    void WebView2HostWebRenderer::SetCursorChangeHandler(CursorChangeHandler) {}
    void WebView2HostWebRenderer::SetBackHandler(BackHandler) {}
    void WebView2HostWebRenderer::RequestBack(std::string_view) {}
    bool WebView2HostWebRenderer::IsCurrentInputPresentation(std::string_view, std::uint64_t) const { return true; }
    void WebView2HostWebRenderer::SetInputFocus(bool focused)
    {
        if (focused) CHECK(!env.settings.blocks.empty() && env.engineFocus);
        env.browserFocus = focused;
    }
    void WebView2HostWebRenderer::InjectKeyEvent(std::uint32_t, bool) {}
    void WebView2HostWebRenderer::InjectKeyboard(const osfui::wv2::msg::Keyboard& key) { env.keys.push_back(key.vk); }
    void WebView2HostWebRenderer::InjectText(const osfui::wv2::msg::TextInput&) {}
    void WebView2HostWebRenderer::SetWindowActive(bool) {}
    void WebView2HostWebRenderer::GameWindowChanged() {}
    void WebView2HostWebRenderer::InjectMouseMove(int, int) {}
    void WebView2HostWebRenderer::InjectMouseButton(int, int, int, bool) {}
    void WebView2HostWebRenderer::InjectMouseWheel(int, int, int) {}
    void WebView2HostWebRenderer::InjectPhysicalMouseWheel(int, int, int) {}
    void WebView2HostWebRenderer::OpenDevTools(std::string_view) {}
    void WebView2HostWebRenderer::SetConsoleHandler(std::string_view, ConsoleHandler) {}
    void WebView2HostWebRenderer::SetViewHidden(std::string_view view, bool hidden, bool) { env.hidden[std::string(view)] = hidden; }
    void WebView2HostWebRenderer::SetViewOrder(std::string_view, int) {}
    void WebView2HostWebRenderer::DestroyView(std::string_view view) { env.hidden.erase(std::string(view)); }

    // Capture/GPU readiness is supplied by the test; no GPU completion is simulated.
    SharedFrameConsumer::~SharedFrameConsumer() = default;
    struct D3D12Compositor::Impl {};
    D3D12Compositor::D3D12Compositor(std::shared_ptr<SharedFrameConsumer>) : m_impl(std::make_unique<Impl>()) { ++env.compositorCreations; }
    D3D12Compositor::~D3D12Compositor() = default;
    void D3D12Compositor::Update() {}
    void D3D12Compositor::SetVisible(bool visible) { env.compositorVisible = visible; }
    bool D3D12Compositor::PreparedFrameReady(std::uint32_t, std::uint32_t) const { return env.preparedFrameReady; }

    bool MenuEventSink::Install() { ++env.menuEventInstalls; return env.menuEventsInstall; }
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
        std::optional<ClientSize> GameWindowClientSize() { return std::nullopt; }
    }
    namespace HardwareCursor { void SetSystemCursorId(std::uint32_t) {} }
    XInputPoller::State XInputPoller::Poll(double) { return {}; }
    void XInputPoller::Reset() {}

    EngineD3D12 LocateEngineD3D12()
    {
        static ID3D12Device device;
        static ID3D12CommandQueue queue;
        return { &device, &queue };
    }

    // No file worker or legacy/VM services run in this suite. Their adapters have separate tests.
    DevViewReloadWorker::DevViewReloadWorker(std::filesystem::path, Refresh) {}
    DevViewReloadWorker::~DevViewReloadWorker() = default;
    void DevViewReloadWorker::SetMods(std::vector<std::string>) {}
    std::vector<std::string> DevViewReloadWorker::DrainCompleted() { return {}; }
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
