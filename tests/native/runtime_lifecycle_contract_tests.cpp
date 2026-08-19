
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
	int failures = 0;

	void Check(const bool a_condition, const char* a_message)
	{
		if (!a_condition) {
			std::cerr << "FAIL: " << a_message << '\n';
			++failures;
		}
	}

	std::string ReadSource(const char* a_path)
	{
		std::ifstream input(a_path, std::ios::binary);
		if (!input) {
			std::cerr << "FAIL: could not read " << a_path << '\n';
			++failures;
			return {};
		}
		std::ostringstream contents;
		contents << input.rdbuf();
		return contents.str();
	}

	std::string FunctionBody(std::string_view a_source, std::string_view a_signature)
	{
		const auto signature = a_source.find(a_signature);
		if (signature == std::string_view::npos) {
			std::cerr << "FAIL: missing function signature: " << a_signature << '\n';
			++failures;
			return {};
		}
		const auto openingBrace = a_source.find('{', signature + a_signature.size());
		if (openingBrace == std::string_view::npos) {
			std::cerr << "FAIL: missing function body: " << a_signature << '\n';
			++failures;
			return {};
		}

		enum class LexState
		{
			Normal,
			LineComment,
			BlockComment,
			String,
			Character
		};

		LexState state = LexState::Normal;
		int depth = 0;
		for (std::size_t i = openingBrace; i < a_source.size(); ++i) {
			const char c = a_source[i];
			const char next = i + 1 < a_source.size() ? a_source[i + 1] : '\0';
			switch (state) {
			case LexState::LineComment:
				if (c == '\n') state = LexState::Normal;
				continue;
			case LexState::BlockComment:
				if (c == '*' && next == '/') {
					state = LexState::Normal;
					++i;
				}
				continue;
			case LexState::String:
				if (c == '\\') {
					++i;
				} else if (c == '"') {
					state = LexState::Normal;
				}
				continue;
			case LexState::Character:
				if (c == '\\') {
					++i;
				} else if (c == '\'') {
					state = LexState::Normal;
				}
				continue;
			case LexState::Normal:
				break;
			}

			if (c == '/' && next == '/') {
				state = LexState::LineComment;
				++i;
			} else if (c == '/' && next == '*') {
				state = LexState::BlockComment;
				++i;
			} else if (c == '"') {
				state = LexState::String;
			} else if (c == '\'') {
				state = LexState::Character;
			} else if (c == '{') {
				++depth;
			} else if (c == '}' && --depth == 0) {
				return std::string(a_source.substr(openingBrace, i - openingBrace + 1));
			}
		}

		std::cerr << "FAIL: unterminated function body: " << a_signature << '\n';
		++failures;
		return {};
	}

	bool ContainsInOrder(std::string_view a_text, std::initializer_list<std::string_view> a_needles)
	{
		std::size_t position = 0;
		for (const auto needle : a_needles) {
			position = a_text.find(needle, position);
			if (position == std::string_view::npos) {
				return false;
			}
			position += needle.size();
		}
		return true;
	}

	std::size_t Count(std::string_view a_text, std::string_view a_needle)
	{
		std::size_t count = 0;
		std::size_t position = 0;
		while ((position = a_text.find(a_needle, position)) != std::string_view::npos) {
			++count;
			position += a_needle.size();
		}
		return count;
	}
}

int main()
{
	const auto runtimeFrameSource = ReadSource("../../src/Runtime/RuntimeFrame.cpp");
	const auto runtimeSource = ReadSource("../../src/Runtime/Runtime.cpp") +
		runtimeFrameSource +
		ReadSource("../../src/Bridge/RuntimeBridge.cpp") +
		ReadSource("../../src/Views/RuntimeViews.cpp") +
		ReadSource("../../src/Input/RuntimeInput.cpp");
	const auto runtimeHeader = ReadSource("../../src/Runtime/Runtime.h");
	const auto runtimeHealthSource = ReadSource("../../src/Runtime/RuntimeHealthCoordinator.cpp");
	const auto pluginSource = ReadSource("../../src/Core/Plugin.cpp");
	const auto papyrusSource = ReadSource("../../src/API/PapyrusApi.cpp");
	const auto bridgeApiSource = ReadSource("../../src/API/BridgeApi.cpp");
	const auto rendererSource = ReadSource("../../src/Render/WebView2HostWebRenderer.cpp");
	const auto compositorSource = ReadSource("../../src/Composite/D3D12Compositor.cpp");
	const auto manifestSource = ReadSource("../../src/Views/ViewManifest.h") +
		ReadSource("../../src/Views/ViewManifest.cpp");
	const auto hostSource = ReadSource("../../tools/webview2_host/HostApp.cpp") +
		ReadSource("../../tools/webview2_host/GameMessages.inl");
	const auto hostGraphics = ReadSource("../../tools/webview2_host/HostGraphics.inl");
	const auto hostMessages = ReadSource("../../tools/webview2_shared/Wv2Messages.h");
	const auto menuEventSource = ReadSource("../../src/Input/MenuEventSink.cpp");
	const auto presentationSource = ReadSource("../../src/Views/ViewPresentationController.cpp");

	const auto initialize = FunctionBody(runtimeSource, "bool Runtime::Initialize()");
	Check(ContainsInOrder(initialize, {
		"InitializePaths()",
		"LoadLocalization()",
		"InitializeSettingsModule()",
		"InitializeFeatureModules()",
		"LoadStartupContent()",
		"InitializeRenderer()" }),
		"paths and localization must initialize before developer settings latch, which must precede manifest discovery and renderer startup");
	Check(initialize.find("_developerMode") != std::string::npos &&
		initialize.find("std::make_unique<DevViewReloadWorker>") != std::string::npos,
		"the loose-view reload worker must be constructed only from the startup-latched developer branch");

	const auto initializeSettings = FunctionBody(runtimeSource, "void Runtime::InitializeSettingsModule()");
	Check(ContainsInOrder(initializeSettings, {
		"_settings = std::make_unique<SettingsModule>",
		"GetValue(\"osfui\", \"developerMode\")",
		"_developerMode = configured->get<bool>()",
		"Log::SetDebugLogging(_developerMode)" }),
		"the one authoritative SettingsModule must supply and latch developer mode before debug logging is configured");
	Check(initializeSettings.find("_developerMode = false") != std::string::npos,
		"an unavailable or malformed developer setting must fail closed");
	Check(initializeSettings.find("GetValue(\"osfui\", \"highRefreshCapture\")") !=
			std::string::npos &&
		initializeSettings.find("_highRefreshCapture = false") != std::string::npos,
		"240 Hz capture must be a separate off-by-default startup latch");

	const auto initializeRenderer = FunctionBody(runtimeSource, "bool Runtime::InitializeRenderer()");
	Check(initializeRenderer.find(".devMode = _developerMode") != std::string::npos,
		"the WebView host must receive the effective startup latch");
	Check(initializeRenderer.find(".highRefreshCapture = _highRefreshCapture") !=
		std::string::npos,
		"the WebView host must receive the explicit high-refresh capture opt-in");

	const auto onSettingChanged = FunctionBody(runtimeSource,
		"void Runtime::OnSettingChanged(std::string_view a_modId");
	Check(onSettingChanged.find("a_key == \"developerMode\"") != std::string::npos &&
		onSettingChanged.find("desired != _developerMode") != std::string::npos &&
		onSettingChanged.find("_developerMode = desired") == std::string::npos &&
		onSettingChanged.find("SetDebugLogging") == std::string::npos,
		"changing developerMode must report a next-launch preference without mutating the effective session");
	Check(onSettingChanged.find("a_key == \"highRefreshCapture\"") != std::string::npos &&
		onSettingChanged.find("desired != _highRefreshCapture") != std::string::npos &&
		onSettingChanged.find("_highRefreshCapture = desired") == std::string::npos,
		"changing highRefreshCapture must not mutate the effective session");

	const auto captureCadence = FunctionBody(hostSource, "void ApplyCaptureCadence()");
	Check(captureCadence.find(
		"highRefreshCapture && focusGranted ? 240u : 60u") != std::string::npos,
		"focused capture must default to 60 Hz and require an explicit opt-in for 240 Hz");
	const auto frameArrival = FunctionBody(hostSource, "void OnFrameArrived(");
	Check(ContainsInOrder(frameArrival, {
		"TryGetNextFrame()",
		"if (!captureHasVisibleView.load(std::memory_order_acquire)) return",
		"capturedFrame.Surface()" }),
		"hidden capture must drain WGC and return before accessing the surface");
	const auto publishFrame = FunctionBody(hostGraphics, "void PublishFrame(");
	Check(ContainsInOrder(publishFrame, {
		"const bool ringNeedsRebuild",
		"if (!ringNeedsRebuild && frameSerial != 0 && consumed() < frameSerial) return",
		"EnsureRing(a_width, a_height)",
		"context->CopyResource" }),
		"capture must pace same-sized frames while allowing a resized ring to replace an unpresentable first frame");

	Check(ContainsInOrder(runtimeFrameSource, {
		"void Runtime::ProcessRendererFrame(double a_deltaSeconds)",
		"OverlayInputHook::GameWindowClientSize()",
		"OnOutputResized(clientSize->width, clientSize->height)",
		"else if (_compositor)",
		"_compositor->GetObservedOutputSize()" }),
		"the game client area must size the browser before transient Scaleform targets, with compositor observation retained only as fallback");

	const auto settingsMaintenance = FunctionBody(runtimeSource,
		"void Runtime::ProcessSettingsMaintenance()");
	Check(ContainsInOrder(settingsMaintenance, {
		"PumpPersistence(_uptime)",
		"if (_developerMode)",
		"PumpSchemaHotReload(_uptime)" }),
		"schema hot reload must run only in effective developer mode while persistence always runs");

	const auto gameWindowKey = FunctionBody(runtimeSource,
		"bool Runtime::OnGameWindowKey(std::uint32_t a_vkCode");
	const auto nativeAccelerator = FunctionBody(runtimeSource,
		"bool Runtime::OnNativeAcceleratorKey(std::uint32_t a_vkCode");
	Check(gameWindowKey.find("_developerMode && a_vkCode == kVkF12") != std::string::npos &&
		gameWindowKey.find("_devToolsRequested.store(true)") != std::string::npos &&
		nativeAccelerator.find("_developerMode && a_vkCode == kVkF12") != std::string::npos,
		"F12 must be framework-owned and request DevTools only in effective developer mode");

	const auto instantiateView = FunctionBody(runtimeSource,
		"bool Runtime::InstantiateView(const ViewManifest& a_manifest");
	Check(instantiateView.find("if (_developerMode)") != std::string::npos &&
		instantiateView.find("SetConsoleHandler") != std::string::npos,
		"browser console forwarding must be installed only in effective developer mode");
	Check(ContainsInOrder(instantiateView, {
		"SetBridgeAvailability(_bridge.get())",
		"_bridge->OnViewCreated" }) &&
		instantiateView.find("permissions") == std::string::npos,
		"every instantiated view must receive the native bridge without a manifest permission gate");
	Check(manifestSource.find("ViewPermissions") == std::string::npos &&
		manifestSource.find("\"permissions\"") == std::string::npos &&
		hostMessages.find("F(\"bridge\"") == std::string::npos,
		"the removed permissions model and per-view bridge wire flag must not return");

	const auto hudEligible = FunctionBody(runtimeSource,
		"bool Runtime::HudAutoStartEligible(const ViewManifest& a_manifest) const");
	const auto viewsData = FunctionBody(runtimeSource, "nlohmann::json Runtime::BuildViewsData() const");
	Check(hudEligible.find("!a_manifest.debugOnly || _developerMode") != std::string::npos &&
		viewsData.find("!m.debugOnly || _developerMode") != std::string::npos,
		"debugOnly views must remain discovered but gain catalog and HUD eligibility only in effective developer mode");

	const auto protocolFault = FunctionBody(runtimeSource,
		"void Runtime::OnProtocolFault(std::string_view a_viewId");
	Check(protocolFault.find("_developerMode && _bridge && !a_viewId.empty()") != std::string::npos &&
		protocolFault.find("osfui.debug.error") != std::string::npos,
		"per-view protocol diagnostics must be a developer-mode-only event");

	Check(runtimeHeader.find("_developerMode{ false }") != std::string::npos &&
		runtimeHealthSource.find("{ \"devMode\", runtime._developerMode }") != std::string::npos,
		"System Health must publish the effective runtime latch, defaulting fail closed");
	const auto healthPump = FunctionBody(runtimeHealthSource,
		"void RuntimeHealthCoordinator::Pump()");
	Check(healthPump.find("Store().Generation()") != std::string::npos &&
		healthPump.find("LoadErrorGeneration") == std::string::npos,
		"System Health must use the single SettingsStore generation for registry and load-error changes");
	Check(runtimeSource.find("Log::DevMode") == std::string::npos &&
		runtimeSource.find("_config.devMode") == std::string::npos,
		"runtime feature policy must not be owned by the logging namespace or a removed config object");

	const auto tick = FunctionBody(runtimeSource, "void Runtime::Tick(double a_deltaSeconds)");
	const auto frameTaskRun = FunctionBody(pluginSource, "void Run() override");
	Check(ContainsInOrder(frameTaskRun, {
		"!Runtime::Get().IsVisible()",
		"nowTicks < m_nextIdleTick.load",
		"m_tickPending.exchange",
		"NativeMainThreadQueue::Post" }),
		"hidden frames must return before coalescing or allocating an engine delegate");
	Check(pluginSource.find("kIdleTickInterval = std::chrono::milliseconds(100)") != std::string::npos,
		"hidden runtime maintenance must be capped at 10 Hz while visible UI remains per-frame");
	Check(papyrusSource.find("State().pending.exchange(false") != std::string::npos &&
		FunctionBody(papyrusSource, "PendingBatch TakePendingBatch(").find("std::lock_guard") != std::string::npos,
		"Papyrus work must use one pending probe and one batch lock");
	Check(bridgeApiSource.find("BridgeApi::PendingBatch BridgeApi::TakePendingBatch()") != std::string::npos &&
		FunctionBody(bridgeApiSource, "BridgeApi::PendingBatch BridgeApi::TakePendingBatch()")
			.find("std::lock_guard") != std::string::npos,
		"Bridge runtime queues must be extracted as one batch");
	Check(ContainsInOrder(tick, {
		"ProcessPauseMenuEntry()",
		"auto bridgeBatch = API::BridgeApi::Get().TakePendingBatch()",
		"TakePresentationRequests(std::move(bridgeBatch.presentation))",
		"PreparePresentationRequests(presentationWork)",
		"auto papyrusBatch = API::Papyrus::TakePendingBatch()",
		"ProcessBackendQueues(std::move(papyrusBatch)",
		"ApplyPresentationRequests(presentationWork)" }),
		"Tick must instantiate requested views hidden, flush native sends, then apply visibility");
	const auto pauseMenuEntry = FunctionBody(runtimeSource, "void Runtime::ProcessPauseMenuEntry()");
	Check(ContainsInOrder(pauseMenuEntry, {
		"PauseMenuEntry::TakeOpenRequest()",
		"UI_MESSAGE_TYPE::kHide",
		"EnqueueOpenView(std::string(Ids::kSettingsViewId))" }),
		"PauseMenu actions must defer the engine hide and Mod Settings open to Runtime::Tick");
	const auto backendQueues = FunctionBody(runtimeSource, "void Runtime::ProcessBackendQueues(");
	Check(backendQueues.find("API::BridgeApi::Get().PumpMainThread()") != std::string::npos,
		"the extracted backend phase must still flush native sends before presentation is applied");

	const auto applyRequests = FunctionBody(runtimeSource,
		"void Runtime::ApplyPresentationRequests(const PendingPresentationWork& a_work)");
	Check(ContainsInOrder(applyRequests, {
		"case ViewPresentationRequest::ToggleDefault:",
		"CancelPendingOpen()",
		"case ViewPresentationRequest::Back:",
		"CancelPendingOpen()",
		"case ViewPresentationRequest::CloseAll:",
		"CancelPendingOpen()",
		"_presentation.CloseAll()" }),
		"toggle, Escape/Back, and CloseAll must cancel a pending open");
	Check(ContainsInOrder(applyRequests, {
		"if (r.open)",
		"*_pendingViewOpen == r.view",
		"CancelPendingOpen()",
		"_presentation.Close(r.view)" }),
		"a native close request must cancel its pending open before closing the view");

	const auto beginOpen = FunctionBody(runtimeSource, "bool Runtime::BeginViewOpen(std::string_view a_id)");
	Check(ContainsInOrder(beginOpen, {
		"manifest->kind == ViewKind::Hud",
		"return _presentation.Open(a_id)",
		"const auto loadState = m_viewLoads.GetState(a_id)",
		"loadState == ViewLoadState::Finished",
		"return _presentation.Open(a_id)",
		"_pendingViewOpen = std::string(a_id)" }),
		"menus must stay closed while loading; only HUDs and load-complete menus open immediately");

	const auto cancelPending = FunctionBody(runtimeSource, "bool Runtime::CancelPendingOpen()");
	Check(ContainsInOrder(cancelPending, {
		"const auto target = std::move(*_pendingViewOpen)",
		"_pendingViewOpen.reset()" }),
		"pending-open cancellation must drop ownership without changing presentation");
	const auto drivePending = FunctionBody(runtimeSource, "void Runtime::DrivePendingOpen()");
	Check(ContainsInOrder(drivePending, {
		"_presentation.IsInstantiated(target)",
		"m_viewLoads.GetState(target) != ViewLoadState::Finished",
		"_presentation.Open(target)",
		"_pendingViewOpen.reset()",
		"ApplyViewPresentationPolicy()" }),
		"a pending menu must remain closed until main-frame load succeeds, then enter normal presentation policy");

	const auto endpoints = FunctionBody(runtimeSource,
		"void Runtime::RegisterPlatformEndpoints(MessageBridge& a_bridge)");
	Check(ContainsInOrder(endpoints, {
		"RegisterSend(\"close\"",
		"*_pendingViewOpen == source",
		"CancelPendingOpen()",
		"_presentation.Close(source)" }),
		"browser close must cancel a pending open or close the calling view");
	Check(ContainsInOrder(endpoints, {
		"RegisterRequest(\"menu.close\"",
		"*_pendingViewOpen == id",
		"cancelled = CancelPendingOpen()",
		"_presentation.Close(id)" }),
		"browser menu.close must cancel a pending open before applying the closed state");
	Check(ContainsInOrder(endpoints, {
		"const auto* manifest = _views.Find(id)",
		"if (!manifest)",
		"Reject(\"unknown-view\"",
		"!_captureIntegrationAvailable",
		"Reject(\"input-unavailable\"",
		"EnqueueOpenView(std::move(id))" }),
		"browser menu.open must fail closed before an unknown or unsafe menu is queued");
	Check(ContainsInOrder(endpoints, {
		"RegisterRequest(\"setViewHidden\"",
		"_presentation.IsInstantiated(id)",
		"const bool hidden = Json::Get(a_p, \"hidden\", false)",
		"*_pendingViewOpen == id",
		"CancelPendingOpen()",
		"_presentation.Close(id)",
		"BeginViewOpen(id)",
		"ApplyViewPresentationPolicy()" }),
		"setViewHidden must transition the presentation model rather than bypassing it at the renderer");
	Check(runtimeSource.find("bool Runtime::SetViewHidden") == std::string::npos &&
		runtimeHeader.find("bool SetViewHidden") == std::string::npos,
		"setViewHidden must not retain a second Runtime visibility authority");

	const auto applyPolicy = FunctionBody(runtimeSource, "void Runtime::ApplyViewPresentationPolicy()");
	Check(ContainsInOrder(applyPolicy, {
		"DesiredCapture() && !_captureIntegrationAvailable",
		"CancelPendingOpen()",
		"_presentation.CloseActiveMenu()" }),
		"central presentation policy must fail closed when capture integration is unavailable");
	Check(ContainsInOrder(applyPolicy, {
		"visible && !wasVisible",
		"m_viewReveal.Arm()" }),
		"a cold or reopened presentation must arm the fresh-frame reveal gate");
	Check(ContainsInOrder(applyPolicy, {
		"if (!visible)",
		"m_viewReveal.Cancel()" }),
		"closing during a held reveal must cancel the reveal gate");

	const auto setHidden = FunctionBody(rendererSource,
		"void WebView2HostWebRenderer::SetViewHidden(std::string_view a_viewId, bool a_hidden)");
	Check(ContainsInOrder(setHidden, {
		"wasAllHidden",
		"RecomputeAllHidden()",
		"wasAllHidden && !_impl->allHidden",
		"++_impl->presentationEpoch",
		"_impl->haveFrame = false",
		".presentationEpoch = presentation" }),
		"closed-to-open must advance the host presentation epoch and invalidate the cached frame");

	const auto onFrame = FunctionBody(rendererSource, "void OnFrameMessage(const msg::Frame& a_msg)");
	Check(ContainsInOrder(onFrame, {
		"allHidden || presentation != presentationEpoch",
		"haveFrame = false",
		"ackNew = true" }),
		"all-hidden and wrong-epoch frames must be rejected and acknowledged");
	const auto takeLatestFrame = FunctionBody(rendererSource,
		"std::optional<FrameBufferView> WebView2HostWebRenderer::TakeLatestFrame()");
	Check(takeLatestFrame.find("submittedRingGeneration == _impl->sharedRingGeneration") != std::string::npos &&
		takeLatestFrame.find("submittedSerial == _impl->frameSerial") != std::string::npos &&
		Count(takeLatestFrame, "return std::nullopt") == 2,
		"the renderer must hand each ring-generation/frame-serial pair to Runtime only once");
	Check(ContainsInOrder(takeLatestFrame, {
		"_impl->submittedRingGeneration = _impl->sharedRingGeneration",
		"_impl->submittedSerial = _impl->frameSerial",
		".ringGeneration = _impl->sharedRingGeneration" }),
		"the renderer must publish generation-aware frame identity after consuming the edge");

	const auto submitFrame = FunctionBody(runtimeSource, "void Runtime::SubmitFrameIfVisible()");
	Check(ContainsInOrder(submitFrame, {
		"_renderer->TakeLatestFrame()",
		"_latestFrame = *frame",
		"m_viewReveal.Observe(observation, _uptime)",
		"if (decision.submitFrame && frame)",
		"_compositor->Submit(*frame)",
		"_compositor->PrepareSharedRing()",
		"if (decision.reveal)",
		"_compositor->SetVisible(true)" }),
		"Runtime must submit the gate-approved frame before making the compositor visible");
	const auto prepareSharedRing = FunctionBody(compositorSource,
		"void D3D12Compositor::PrepareSharedRing()");
	Check(ContainsInOrder(prepareSharedRing, {
		"!_impl->sharedRing.pendingDirty",
		"return",
		"_impl->EnsureSetup()",
		"_impl->EnsureSharedRing()" }),
		"deferred ring adoption must retry without locking when no generation is pending");
	const auto cacheFrame = FunctionBody(compositorSource,
		"void CacheFrame(const FrameBufferView& a_frame)");
	Check(ContainsInOrder(cacheFrame, {
		"a_frame.ringGeneration == lastSubmittedGeneration",
		"a_frame.frameIndex == lastSubmittedIndex",
		"lastSubmittedGeneration = a_frame.ringGeneration",
		"frameGeneration = a_frame.ringGeneration" }),
		"compositor frame caching must distinguish identical serials from different rings");
	const auto recordOverlay = FunctionBody(compositorSource,
		"bool RecordOverlay(ID3D12GraphicsCommandList* a_list");
	Check(ContainsInOrder(recordOverlay, {
		"frameGeneration == sharedRing.activeGeneration",
		"ringSlot = sharedRing.readySlot",
		"DrawInstanced(3, 1, 0, 0)" }),
		"each UI target must draw the cached frame while only generation-matched candidates replace it");
	Check(ContainsInOrder(submitFrame, {
		"if (!decision.timedOut)",
		"CancelPendingOpen()",
		"_presentation.CloseAll()",
		"ApplyViewPresentationPolicy()",
		"ReconcileFocusMenu()",
		"ReconcileControlLayer()",
		"ReconcileSimPause()",
		"FreeCursor::Apply(false)" }),
		"reveal timeout must close the presentation and immediately release every engine-owned input edge");

	const auto menuEvent = FunctionBody(menuEventSource, "MenuEventSink::ProcessEvent(");
	Check(ContainsInOrder(menuEvent, {
		"name == \"LoadingMenu\"",
		"name == \"MainMenu\"",
		"PresentationRequest::CloseAll" }),
		"LoadingMenu and MainMenu must enqueue the same CloseAll transition");

	const auto open = FunctionBody(presentationSource,
		"bool ViewPresentationController::Open(std::string_view a_id)");
	Check(open.find("_activeMenu = id") != std::string::npos &&
		open.find("_hudShown.clear()") == std::string::npos,
		"replacing the active menu must preserve open HUDs");

	const auto close = FunctionBody(presentationSource,
		"bool ViewPresentationController::Close(std::string_view a_id)");
	Check(close.find("_hudShown.erase(id)") != std::string::npos &&
		close.find("_activeMenu.reset()") != std::string::npos &&
		close.find("RemoveInstantiated") == std::string::npos &&
		close.find("DestroyView") == std::string::npos,
		"ordinary close must hide logical presentation state without destroying the reusable document");

	const auto closeAll = FunctionBody(presentationSource, "void ViewPresentationController::CloseAll()");
	Check(ContainsInOrder(closeAll, { "_activeMenu.reset()", "_hudShown.clear()" }),
		"CloseAll must close both the active menu and every HUD");

	const auto startup = FunctionBody(runtimeSource, "void Runtime::InitializeStartupViews()");
	Check(startup.find("PrewarmView") == std::string::npos &&
		ContainsInOrder(startup, {
			"manifest.kind != ViewKind::Hud",
			"HudAutoStartEligible(manifest)",
			"_viewPolicy.HudAutoStart",
			"InstantiateView(manifest, \"for HUD auto-start\")" }),
		"startup must instantiate only HUDs whose effective auto-start policy is on");

	Check(runtimeSource.find("DriveViewLifecycle") == std::string::npos &&
		runtimeSource.find("IdleReclaim") == std::string::npos,
		"ordinary closed documents must have no timed suspension or reclamation driver");
	Check(rendererSource.find("SuspendView") == std::string::npos &&
		hostMessages.find("suspendView") == std::string::npos &&
		hostSource.find("TrySuspend") == std::string::npos,
		"the game-host boundary must not carry an asynchronous idle-suspension state machine");
	Check(rendererSource.find("PrewarmView") == std::string::npos &&
		hostMessages.find("prewarm") == std::string::npos &&
		hostSource.find("Prewarm") == std::string::npos,
		"unused menus must not be instantiated or painted through a prewarm path");
	const auto finishControllerSetup = FunctionBody(hostSource,
		"void FinishControllerSetup(View& a_view)");
	Check(ContainsInOrder(finishControllerSetup, {
		"InstallEvents(a_view)",
		"InstallBridgeShim(a_view)" }) &&
		finishControllerSetup.find("a_view.bridge") == std::string::npos,
		"the browser host must inject the bridge shim for every view");

	const auto onViewLoad = FunctionBody(runtimeSource,
		"void Runtime::OnViewLoad(std::string_view a_viewId");
	Check(ContainsInOrder(onViewLoad, {
		"if(recovery.exhausted)",
		"TearDownFailedView(id)" }),
		"explicit teardown must remain limited to a document that exhausts load recovery");
	const auto teardownFailed = FunctionBody(runtimeSource,
		"void Runtime::TearDownFailedView(const std::string& a_id)");
	Check(teardownFailed.find("_renderer->DestroyView(a_id)") != std::string::npos &&
		teardownFailed.find("IdleReclaim") == std::string::npos,
		"terminally failed documents may be destroyed without creating an idle-reclaim policy");

	Check(Count(applyRequests, "CancelPendingOpen()") >= 4,
		"all four queued close paths must retain pending-open cancellation");

	if (failures == 0) {
		std::cout << "runtime_lifecycle_contract_tests: ok\n";
	}
	return failures;
}
