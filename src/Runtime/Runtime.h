#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "API/BridgeApi.h"
#include "API/PapyrusApi.h"
#include "Bridge/MessageBridge.h"
#include "Bridge/RetainedStateStore.h"
#include "Composite/D3D12Compositor.h"
#include "Dependency/OSFSettingsClient.h"
#include "Input/GamepadSession.h"
#include "Input/InputCaptureController.h"
#include "Input/PointerInputState.h"
#include "Input/RelativePointerSession.h"
#include "Input/ViewInputGrants.h"
#include "Input/XInputPoller.h"
#include "Render/BrowserHostRecovery.h"
#include "Render/WebView2HostWebRenderer.h"
#include "Views/Dev/DevViewReloadWorker.h"
#include "Views/ViewLoadTracker.h"
#include "Views/ViewManager.h"
#include "Views/ViewOpenCoordinator.h"
#include "Views/ViewPresentationController.h"
#include "Views/ViewRecoveryTracker.h"
#include "Views/ViewRevealGate.h"
#include "Views/ViewRequestQueue.h"

namespace OSFUI
{
	class Runtime
	{
	public:
		static Runtime& Get();

		// SFSE lifecycle and post-menu-advance update entry points.
		bool Initialize();
		void OnPostLoad();
		void OnPostPostDataLoad();
		void Update();

		// Published state, safe to query from the window-message thread.
		bool IsVisible() const;
		bool IsInputCaptured() const;

		// Queued requests; applied on the runtime tick.
		void EnqueuePresentationRequest(ViewPresentationRequest a_req);
		void EnqueueOpenView(std::string a_viewId);
		void EnqueueCloseView(std::string a_viewId);
		// Browser transport threads only enqueue this ownership edge; Runtime applies
		// it beside presentation work so native callbacks execute in the runtime update.
		void EnqueueRelativePointerCapture(std::string a_viewId, bool a_active);

		// WndProc input entry points (window-message thread).
		bool OnGameWindowKeyboard(const osfui::wv2::msg::Keyboard& a_key);
		void OnGameWindowText(const osfui::wv2::msg::TextInput& a_text);
		void OnGameWindowActivation(bool a_active);
		void OnGameWindowMouseAbsolute(int a_clientX, int a_clientY, int a_clientW, int a_clientH);
		// Accumulate one packet for the active relative-pointer owner.
		bool OnGameWindowMouseRelative(int a_dx, int a_dy);
		void OnGameWindowMouseButton(int a_button, bool a_down);
		void OnGameWindowMouseWheel(int a_wheelDelta);

	private:
		Runtime() = default;

		// Independent setup, peer services, then runtime engine integration.
		void LoadStartupContent();
		void InitializeEngineIntegration();
		bool InitializeWebRuntime();
		bool InitializeRenderer();
		void WireRendererLifecycleCallbacks();
		bool InitializeCompositor();
		void InitializeBridge();
		void InitializeStartupViews();

		// Frame stages (RuntimeFrame.cpp).
		void ProcessLifecycleWork();
		void ProcessBackendState(const API::Papyrus::PendingBatch& a_papyrus, const std::vector<API::BridgeApi::ViewStateOp>& a_bridgeState);
		void ApplyNativeState(const std::vector<API::BridgeApi::ViewStateOp>& a_state);
		void ProcessBackendMessages(const API::Papyrus::PendingBatch& a_papyrus);
		void ReconcileFrameState();
		void CommitPresentation();
		void ProcessRendererNotifications();
		void ProcessRendererFrame();

		// View requests, presentation and output geometry.
		void ApplyPresentationRequests(const std::vector<ViewRequestQueue::Operation>& a_requests);
		void PrepareViewOpen(std::string_view a_id, std::string_view a_reason = "on demand", std::optional<std::chrono::steady_clock::time_point> a_requestedAt = std::nullopt);
		void DrivePendingOpen();
		ViewOpenCoordinator::Readiness ViewOpenReadiness(std::string_view a_id) const;
		void DrainViewRegistrations(const std::vector<std::string>& a_ids);
		void ApplyViewPresentationPolicy();
		void OnOutputResized(std::uint32_t a_width, std::uint32_t a_height);
		void UpdateViewReveal();

		// View loading and per-view recovery (Views/RuntimeViews.cpp).
		bool InstantiateView(const ViewManifest& a_manifest, std::string_view a_reason);
		void NavigateView(const ViewManifest& a_manifest);
		void OnViewLoad(std::string_view a_viewId, bool a_failed, std::string_view a_url,
			std::string_view a_description, int a_errorCode);
		bool HudAutoStartEligible(const ViewManifest& a_manifest) const;
		void TearDownFailedView(const std::string& a_id);
		void DriveRecovery();

		// Browser-host recovery; distinct from a single view failing to load.
		void OnRendererFailure(const WebView2HostWebRenderer::FailureEvent& a_event);
		// Deferred until the failure callback has returned; recreates every instantiated view.
		void DriveBrowserHostRecovery();
		void RehydrateRendererAfterRestart();

		// Input policy and routing (Input/RuntimeInput.cpp).
		// Runtime applies presentation failure policy around capture reconciliation.
		void ReconcileInputSuppression();
		void ReconcileFocusMenu();
		// Poll XInput and deliver events to the active document (`ui.gamepad` events).
		void RouteGamepadInput();

		// Relative-pointer session; callbacks run on the runtime tick.
		void ApplyRelativePointerRequest(const ViewRequestQueue::RelativePointerRequest& a_request);

		// Bridge endpoints, retained state and protocol lifecycle (Bridge/RuntimeBridge.cpp).
		void RegisterPlatformEndpoints(MessageBridge& a_bridge);
		void BroadcastViewsData();
		std::unordered_set<std::string> InstantiatedViewsOfMod(std::string_view a_mod) const;
		void PublishModState(std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value);
		void PublishViewsState(std::string_view a_viewId = {});
		void OnViewGreeted(std::string_view a_viewId);
		void OnProtocolFault(std::string_view a_viewId, std::string_view a_code, std::string_view a_message, const nlohmann::json& a_detail, bool a_viewFault);

		// Developer tools and view diagnostics.
		void DriveDevTools();
		void PumpDevViewReload();
		nlohmann::json BuildViewsData() const;

		// Ownership: ordinary mutable fields below belong to the runtime tick.
		// Startup initializes paths/catalog/settings before input hooks are installed;
		// m_initialized, m_developerMode and the renderer pointer then stay stable.
		// Renderer load/failure callbacks run when its queues drain on that tick.

		// Owned services. Keep their construction/destruction order explicit.
		ViewManager m_views;
		std::unique_ptr<WebView2HostWebRenderer> m_renderer;
		std::unique_ptr<D3D12Compositor> m_compositor;
		std::unique_ptr<MessageBridge> m_bridge;
		OSFSettingsClient m_osfSettings;
		// Runtime owns the worker; its synchronized interface owns cross-thread jobs.
		std::unique_ptr<DevViewReloadWorker> m_devViewReload;

		// Startup and frame lifecycle.
		bool m_initialized{ false };
		bool m_webRuntimeReady{ false };
		bool m_developerMode{ false };       // startup-latched; changes apply next launch
		// Monotonic seconds sampled at the start of each update, never accumulated or clamped.
		double m_nowSeconds{ 0.0 };
		// SFSE lifecycle producer -> runtime consumer.
		std::atomic_bool m_engineIntegrationPending{ false };

		// View lifecycle and presentation.
		ViewPresentationController m_presentation;
		ViewOpenCoordinator m_viewOpens;
		ViewLoadTracker m_viewLoads;
		ViewRevealGate m_viewReveal;
		ViewRecoveryTracker m_viewRecovery;
		std::string m_lastShownView;
		std::atomic_bool m_visible{ false };  // runtime -> WndProc/frame-event producer

		// Browser-host recovery.
		BrowserHostRecovery m_browserHostRecovery;

		// Input components own their state and cross-thread publication boundaries.
		InputCaptureController m_inputCapture;
		PointerInputState m_pointerInput;
		RelativePointerSession m_relativePointer;
		ViewInputGrants m_viewInputGrants;
		XInputPoller m_gamepadSource;
		GamepadSession m_gamepadSession;

		// Bridge state and protocol diagnostics.
		RetainedStateStore m_retainedState;
		std::unordered_map<std::string, std::uint32_t> m_viewProtocolFaultCounts;
		std::string m_lastViewsData;

		// Developer tools request (WndProc -> runtime).
		std::atomic_bool m_devToolsRequested{ false };
	};
}
