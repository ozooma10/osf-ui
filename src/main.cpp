#include "composite/D3D12Compositor.h"
#include "composite/ScaleformOverlayHook.h"
#include "input/ControlLayer.h"
#include "input/OverlayInputHook.h"
#include "input/SimPause.h"
#include "render/WebView2HostWebRenderer.h"

#include "v2/Events/Events.h"
#include "v2/Presentation/WebViewPresenter.h"
#include "v2/Scripts/Papyrus.h"

#include "v2/Platform/NativeMainThreadQueue.h"
#include "v2/Runtime/RuntimeCoordinator.h"
#include "v2/Runtime/ViewCatalog.h"
#include "v2/Runtime/ViewDiscovery.h"
#include "v2/Runtime/ViewManifest.h"

namespace
{
    std::atomic_bool g_gameWindowInputInstallPending {false};
    std::atomic_bool g_gameWindowInputRefreshPending {false};

    std::filesystem::path PluginDataDirectory()
    {
        return std::filesystem::path {REX::FModule::GetCurrentModule().GetFileName()}.parent_path() / L"OSFUI";
    }

    std::filesystem::path DefaultViewsDirectory()
    {
        return PluginDataDirectory() / L"Views";
    }

    bool RouteFrameworkKey(std::uint32_t a_virtualKey, bool a_down);

    Presentation::WebViewPresenter& ApplicationPresenter()
    {
        static auto* const presenter = new Presentation::WebViewPresenter {std::make_unique<OSFUI::WebView2HostWebRenderer>(), std::make_unique<OSFUI::D3D12Compositor>(), &OSFUI::ScaleformOverlayHook::DrawEnabled, &RouteFrameworkKey};
        return *presenter;
    }

    void ApplyGameInputCapture(bool a_captured)
    {
        static bool previousCaptured = false;

        OSFUI::ControlLayer::Apply(a_captured);

        if (previousCaptured != a_captured) {
            previousCaptured = a_captured;
            g_gameWindowInputRefreshPending.store(true, std::memory_order_release);
        }
    }

    void ApplyGamePause(bool a_paused)
    {
        OSFUI::SimPause::Apply(a_paused);
    }

    Runtime::RuntimeCoordinator& ApplicationRuntime()
    {
        static Runtime::RuntimeCoordinator runtime {&Papyrus::RegisterFunctions, &ApplicationPresenter(), &ApplyGameInputCapture, &ApplyGamePause};

        return runtime;
    }

    bool RouteFrameworkKey(std::uint32_t a_virtualKey, bool a_down)
    {
        return ApplicationRuntime().RouteKeyEvent(a_virtualKey, a_down);
    }

    bool IsGameWindowInputCaptured()
    {
        return ApplicationRuntime().IsInputCaptured();
    }

    bool RouteGameWindowKey(std::uint32_t a_virtualKey, OSFUI::ScanCode, bool a_down)
    {
        return ApplicationRuntime().RouteKeyEvent(a_virtualKey, a_down);
    }

    void HandleGameWindowKeyboardLayoutChanged()
    {
        // v2 does not currently expose layout-derived key bindings or labels.
    }

    void RouteGameWindowMousePosition(int a_clientX, int a_clientY, int a_clientWidth, int a_clientHeight)
    {
        ApplicationRuntime().RouteMousePosition(a_clientX, a_clientY, a_clientWidth, a_clientHeight);
    }

    void RouteGameWindowMouseButton(int a_button, bool a_down)
    {
        ApplicationRuntime().RouteMouseButtonEvent(a_button, a_down);
    }

    void RouteGameWindowMouseWheel(int a_wheelDelta)
    {
        ApplicationRuntime().RouteMouseWheelEvent(a_wheelDelta);
    }

    void InstallGameWindowInputRouting()
    {
        const bool installed = OSFUI::OverlayInputHook::Install(
            {.isCaptured = &IsGameWindowInputCaptured,
             .onKey = &RouteGameWindowKey,
             .onKeyboardLayoutChanged = &HandleGameWindowKeyboardLayoutChanged,
             .onMouseAbsolute = &RouteGameWindowMousePosition,
             .onMouseButton = &RouteGameWindowMouseButton,
             .onMouseWheel = &RouteGameWindowMouseWheel}
        );

        if (!installed) {
            REX::ERROR("Plugin: game-window input integration is unavailable; capturing v2 menus will be refused");
            return;
        }

        ApplicationRuntime().EnableInputRouting();
    }

    bool RequestViewOpen(std::string a_viewId)
    {
        return ApplicationRuntime().RequestOpenView(std::move(a_viewId));
    }

    bool RequestViewClose(std::string a_viewId)
    {
        return ApplicationRuntime().RequestCloseView(std::move(a_viewId));
    }

    void LoadInstalledViews()
    {
        const auto report = ApplicationRuntime().LoadViews(DefaultViewsDirectory());

        for (const auto& issue : report.issues) {
            REX::ERROR("View discovery failed at '{}': {}", issue.path.string(), issue.message);
        }

        for (const auto& view : ApplicationRuntime().Views().Views()) {
            REX::INFO("Discovered view '{}' title='{}'", view.id, view.title);
        }

        REX::INFO("View discovery completed: {} valid, {} invalid", report.loaded, report.issues.size());
    }

    class RuntimeTickTask final : public SFSE::ITaskDelegate
    {
    public:
        void Run() override
        {
            // AddPermanentTask may run on rotating worker threads. At most one native-main-thread tick may be queued or executing.
            if (_tickPending.exchange(true, std::memory_order_acq_rel)) {
                return;
            }

            try {
                const auto result = Platform::NativeMainThreadQueue::Post(
                    [this] {
                        struct PendingReset
                        {
                            std::atomic_bool& pending;

                            ~PendingReset()
                            {
                                pending.store(false, std::memory_order_release);
                            }
                        };
                        const PendingReset reset {_tickPending};

                        if (g_gameWindowInputInstallPending.exchange(false, std::memory_order_acq_rel)) {
                            InstallGameWindowInputRouting();
                        }

                        ApplicationRuntime().Tick();
                        if (g_gameWindowInputRefreshPending.exchange(false, std::memory_order_acq_rel)) {
                            OSFUI::OverlayInputHook::RequestStateRefresh();
                        }
                    },
                    "OSF UI v2 runtime tick",
                    [this] { ClearPending(); }
                );

                if (result == Platform::NativeMainThreadQueue::PostResult::Unavailable) {
                    // The native queue may not be available during early boot. Let the next permanent-task frame retry.
                    ClearPending();
                }

                _postFailureLogged.store(false, std::memory_order_release);
            } catch (const std::exception& error) {
                ClearPending();

                if (!_postFailureLogged.exchange(true, std::memory_order_acq_rel)) {
                    REX::ERROR("RuntimeTick: failed to post main-thread work: {}", error.what());
                }
            } catch (...) {
                ClearPending();

                if (!_postFailureLogged.exchange(true, std::memory_order_acq_rel)) {
                    REX::ERROR("RuntimeTick: failed to post main-thread work with an unknown exception");
                }
            }
        }

        void Destroy() override
        {
            // Permanent task with process lifetime.
        }

    private:
        void ClearPending()
        {
            _tickPending.store(false, std::memory_order_release);
        }

        std::atomic_bool _tickPending {false};
        std::atomic_bool _postFailureLogged {false};
    };

    void OnSFSEMessage(SFSE::MessagingInterface::Message* a_msg)
    {
        if (!a_msg) {
            return;
        }
        switch (a_msg->type) {
        case SFSE::MessagingInterface::kPostLoad:
            REX::INFO("Plugin: SFSE message kPostLoad");
            ApplicationPresenter().SetDrawPathInstalled(OSFUI::ScaleformOverlayHook::Install());
            break;
        case SFSE::MessagingInterface::kPostDataLoad:
            REX::INFO("Plugin: SFSE message kPostDataLoad");

            ApplicationRuntime().NotifyDataLoaded();
            break;
        case SFSE::MessagingInterface::kPostPostDataLoad:
            REX::INFO("Plugin: SFSE message kPostPostDataLoad");
            g_gameWindowInputInstallPending.store(true, std::memory_order_release);
            if (!Events::Register()) {
                REX::ERROR("Plugin: menu event integration is unavailable");
            }
            break;
        }
    }
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
    SFSE::Init(a_sfse, {.trampoline = true, .trampolineSize = 1024});

    const auto* messaging = SFSE::GetMessagingInterface();
    if (!messaging) {
        REX::ERROR("Plugin: SFSE MessagingInterface is unavailable");
        return false;
    }

    const auto* tasks = SFSE::GetTaskInterface();
    if (!tasks || tasks->Version() < SFSE::TaskInterface::kVersion) {
        REX::ERROR("Plugin: compatible SFSE TaskInterface is unavailable");
        return false;
    }

    Papyrus::SetViewRequestHandlers(&RequestViewOpen, &RequestViewClose);

    try {
        if (!ApplicationPresenter().Initialize(PluginDataDirectory())) {
            REX::ERROR("Plugin: presentation initialization failed");
            return false;
        }

        LoadInstalledViews();
    } catch (const std::exception& error) {
        REX::ERROR("Plugin: view initialization failed: {}", error.what());
        return false;
    } catch (...) {
        REX::ERROR("Plugin: view initialization failed with an unknown exception");
        return false;
    }

    if (!messaging->RegisterListener(OnSFSEMessage)) {
        REX::ERROR("Plugin: failed to register the SFSE message listener");
        return false;
    }

    static RuntimeTickTask runtimeTick;
    tasks->AddPermanentTask(&runtimeTick);

    REX::INFO("Plugin: v2 runtime tick registered through SFSE TaskInterface v{}", tasks->Version());

    return true;
}
