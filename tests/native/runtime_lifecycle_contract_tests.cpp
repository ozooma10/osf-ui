
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
	const auto viewRequestQueueSource = ReadSource("../../src/Views/ViewRequestQueue.cpp");
	const auto rendererSource = ReadSource("../../src/Render/WebView2HostWebRenderer.cpp");
	const auto compositorSource = ReadSource("../../src/Composite/D3D12Compositor.cpp");
	const auto uiPassSource = ReadSource("../../src/Composite/UiPass.cpp");
	const auto manifestSource = ReadSource("../../src/Views/ViewManifest.h") +
		ReadSource("../../src/Views/ViewManifest.cpp");
	const auto hostSource = ReadSource("../../tools/webview2_host/HostApp.cpp") +
		ReadSource("../../tools/webview2_host/GameMessages.inl");
	const auto hostGraphics = ReadSource("../../tools/webview2_host/HostGraphics.inl");
	const auto hostMessages = ReadSource("../../tools/webview2_shared/Wv2Messages.h");
	const auto menuEventSource = ReadSource("../../src/Input/MenuEventSink.cpp");
	const auto overlayInputSource = ReadSource("../../src/Input/OverlayInputHook.cpp");
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
		"const bool newPresentation",
		"if (!ringNeedsRebuild && !newPresentation",
		"EnsureRing(a_width, a_height)",
		"for (std::uint32_t offset = 0; offset < kRingSlots; ++offset)",
		"ring[candidate].lastSerial == 0",
		"consumed(candidate) >= ring[candidate].lastSerial",
		"if (writableSlot == kRingSlots)",
		"context->CopyResource",
		"lastPublishedPresentationEpoch = a_presentationEpoch" }),
		"capture must pace within one presentation while allowing a new presentation to bypass a stranded prior frame through a free slot");
	Check(publishFrame.find("ackedSerials[a_slot]") != std::string::npos &&
		publishFrame.find("consumed(lastSlot) < ring[lastSlot].lastSerial") != std::string::npos &&
		publishFrame.find("ackedSerial.store") == std::string::npos,
		"presentation rollover must not synthetically acknowledge a texture that the GPU may still be reading");
	const auto releaseRing = FunctionBody(hostGraphics, "void ReleaseRing()");
	Check(releaseRing.find("lastPublishedPresentationEpoch = 0") != std::string::npos,
		"rebuilding the shared ring must reset its published-presentation tracker");
	const auto republishLatest = FunctionBody(hostGraphics, "void RepublishLatest()");
	Check(ContainsInOrder(republishLatest, {
		"consumeFences[lastSlot]->GetCompletedValue()",
		"ackedSerials[lastSlot].load()",
		"ring[lastSlot].lastSerial",
		"lastPublishedPresentationEpoch =",
		"presentationEpoch.load(std::memory_order_relaxed)",
		".presentationEpoch = lastPublishedPresentationEpoch" }),
		"cached-frame republication must wait for that slot's prior use and keep presentation-aware pacing synchronized");

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
	Check(ContainsInOrder(gameWindowKey, {
		"ViewTimingClock::now()",
		"_lastToggleRequestNanos.store",
		"EnqueuePresentationRequest(ViewPresentationRequest::ToggleDefault)" }),
		"the toggle input timestamp must be captured before its presentation request is queued");
	Check(ContainsInOrder(viewRequestQueueSource, {
		"void OSFUI::ViewRequestQueue::EnqueueOpen",
		".view = std::move(a_viewId)",
		".requestedAt = std::chrono::steady_clock::now()" }) &&
		ContainsInOrder(FunctionBody(bridgeApiSource, "bool BridgeApi::RequestMenu("), {
			".view = *id",
			".open = a_open",
			".requestedAt = std::chrono::steady_clock::now()" }),
		"internal and native menu requests must capture their timestamps before the Runtime drain");

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

	const auto postDataIntegration = FunctionBody(runtimeSource,
		"void Runtime::InitializePostDataLoadIntegration()");
	Check(ContainsInOrder(postDataIntegration, {
		"_captureIntegrationInitialized = true",
		"UiLayoutGuard::VerifyUiLayout()",
		"_captureIntegrationAvailable = menuEventsInstalled && focusMenuRegistered && inputInstalled",
		"if (!_captureIntegrationAvailable)",
		"_views.Find(Ids::kSettingsViewId)",
		"BeginHiddenPrewarmTiming(settings->id)",
		"InstantiateView(*settings, \"for hidden startup prewarm\")" }) &&
		postDataIntegration.find("_presentation.Open") == std::string::npos &&
		postDataIntegration.find("BeginViewOpen") == std::string::npos,
		"post-post-data-load must record the integration attempt and instantiate settings hidden only after it succeeds");
	Check(runtimeHeader.find("_captureIntegrationInitialized{ false }") != std::string::npos &&
		runtimeHeader.find("_captureIntegrationAvailable{ false }") != std::string::npos,
		"capture integration must distinguish not-yet-initialized from initialized-but-unavailable");
	const auto successfulViewLoad = FunctionBody(runtimeSource, "void Runtime::OnViewLoad(");
	Check(ContainsInOrder(successfulViewLoad, {
		"const auto loadedAt = ViewTimingClock::now()",
		"_coldOpenTiming->loadedAt = loadedAt",
		"FinishHiddenPrewarmTiming(id, loadedAt)" }) &&
		successfulViewLoad.find("Ids::kKeybindingsViewId") == std::string::npos,
		"a hidden settings load must finish timing without cascading into another built-in preload");

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
		"++_mainTickSerial",
		"ProcessPauseMenuEntry()",
		"auto bridgeBatch = API::BridgeApi::Get().TakePendingBatch()",
		"TakePresentationRequests(std::move(bridgeBatch.presentation))",
		"auto papyrusBatch = API::Papyrus::TakePendingBatch()",
		"ProcessBackendQueues(std::move(papyrusBatch)",
		"ApplyPresentationRequests(presentationWork)",
		"ProcessRendererFrame(a_deltaSeconds)" }),
		"Tick must drain retained-state writes before presentation and advance the view-open preflight barrier once per main tick");
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
	Check(runtimeSource.find("PreparePresentationRequests") == std::string::npos &&
		runtimeHeader.find("PreparePresentationRequests") == std::string::npos,
		"requested views must not be instantiated in a preparation pass before the native view-open preflight decision");
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
		"case ViewPresentationRequest::Back:",
		"BackTargetFor(*active)",
		"BeginViewOpen(*target, \"for native back navigation\")",
		"OwnsBackAction(*active)",
		"InjectKeyEvent(kVkEscape, true)" }),
		"a native back target must bypass the synthetic browser round trip");
	Check(ContainsInOrder(applyRequests, {
		"if (r.open)",
		"BeginViewOpen(r.view, \"on demand\", r.requestedAt)",
		"CancelPendingOpen(r.view)",
		"_presentation.Close(r.view)" }),
		"a native close request must cancel its pending open before closing the view");
	Check(ContainsInOrder(applyRequests, {
		"BeginViewOpen(Ids::kSettingsViewId, \"for the default-menu toggle\")",
		"BeginViewOpen(request.view, \"on demand\", request.requestedAt)",
		"BeginViewOpen(r.view, \"on demand\", r.requestedAt)" }),
		"toggle, internal/web/Papyrus opens, and native RequestMenu opens must converge on BeginViewOpen");

	const auto beginOpen = FunctionBody(runtimeSource,
		"bool Runtime::BeginViewOpen(std::string_view a_id, std::string_view a_reason");
	Check(ContainsInOrder(beginOpen, {
		"_presentation.IsOpen(a_id)",
		"_pendingViewOpen && *_pendingViewOpen == a_id",
		"_viewOpenPreflightBarriers.contains",
		"return false",
		"const bool requiresCaptureIntegration",
		"_captureIntegrationInitialized &&",
		"!_captureIntegrationAvailable",
		"required input integration is unavailable",
		"RunViewOpenPreflight(a_id)",
		"ViewOpenPreflightResult::kDenied",
		"current presentation retained",
		"return false",
		"BeginColdOpenTiming(a_id, a_requestedAt)",
		"InstantiateView(*manifest, a_reason)" }),
		"duplicate and ineligible opens must be suppressed before the synchronous callback, and denial must precede instantiation");
	Check(ContainsInOrder(beginOpen, {
		"ViewOpenPreflightResult::kAllowed",
		"InstantiateView(*manifest, a_reason)",
		"_viewOpenPreflightBarriers.emplace(std::string(a_id), _mainTickSerial + 1)",
		"manifest->kind == ViewKind::Hud",
		"requiresStateBarrier || _presentation.Open(a_id)",
		"const bool waitingForCaptureIntegration",
		"!requiresStateBarrier && !waitingForCaptureIntegration && loadState == ViewLoadState::Finished",
		"_pendingViewOpen = std::string(a_id)",
		"native retained-state barrier" }),
		"an approved callback must defer both HUD and menu presentation while ordinary loaded opens retain their immediate path");

	const auto cancelPending = FunctionBody(runtimeSource, "bool Runtime::CancelPendingOpen()");
	Check(ContainsInOrder(cancelPending, {
		"const auto target = std::move(*_pendingViewOpen)",
		"_pendingViewOpen.reset()",
		"_viewOpenPreflightBarriers.erase(target)" }),
		"pending-open cancellation must drop ownership without changing presentation");
	const auto drivePending = FunctionBody(runtimeSource, "void Runtime::DrivePendingOpen()");
	Check(ContainsInOrder(drivePending, {
		"it->second > _mainTickSerial",
		"manifest->kind == ViewKind::Menu",
		"_presentation.Open(it->first)",
		"_viewOpenPreflightBarriers.erase(it)",
		"_presentation.IsInstantiated(target)",
		"requiresCaptureIntegration && !_captureIntegrationInitialized",
		"return",
		"requiresCaptureIntegration && !_captureIntegrationAvailable",
		"CancelPendingOpen()",
		"barrier->second > _mainTickSerial",
		"return",
		"m_viewLoads.GetState(target) != ViewLoadState::Finished",
		"_presentation.Open(target)",
		"_pendingViewOpen.reset()",
		"_viewOpenPreflightBarriers.erase(target)",
		"ApplyViewPresentationPolicy()" }),
		"approved opens must wait at least one main tick and menus must also wait for input and load readiness");

	const auto endpoints = FunctionBody(runtimeSource,
		"void Runtime::RegisterPlatformEndpoints(MessageBridge& a_bridge)");
	Check(ContainsInOrder(endpoints, {
		"RegisterSend(\"close\"",
		"CancelPendingOpen(source)",
		"_presentation.Close(source)" }),
		"browser close must cancel a pending open or close the calling view");
	Check(ContainsInOrder(endpoints, {
		"RegisterRequest(\"menu.close\"",
		"cancelled = CancelPendingOpen(id)",
		"_presentation.Close(id)" }),
		"browser menu.close must cancel a pending open before applying the closed state");
	Check(ContainsInOrder(endpoints, {
		"const auto* manifest = _views.Find(id)",
		"if (!manifest)",
		"Reject(\"unknown-view\"",
		"_captureIntegrationInitialized && !_captureIntegrationAvailable",
		"Reject(\"input-unavailable\"",
		"EnqueueOpenView(std::move(id))" }),
		"browser menu.open must queue while input integration is pending and reject only an initialized failure");
	Check(ContainsInOrder(endpoints, {
		"RegisterRequest(\"setViewHidden\"",
		"_presentation.IsInstantiated(id)",
		"const bool hidden = Json::Get(a_p, \"hidden\", false)",
		"CancelPendingOpen(id)",
		"_presentation.Close(id)",
		"BeginViewOpen(id)",
		"ApplyViewPresentationPolicy()" }),
		"setViewHidden must transition the presentation model rather than bypassing it at the renderer");
	Check(ContainsInOrder(endpoints, {
		"RegisterSend(\"osfui.handleBack\"",
		"Json::Get(a_p, \"view\", \"\")",
		"_views.Find(target)",
		"manifest->kind != ViewKind::Menu",
		"SetBackOwnership(src, handle, target)" }),
		"browser back ownership may register a validated native menu target");
	Check(ContainsInOrder(endpoints, {
		"RegisterSend(\"osfui.relativePointer\"",
		"activeValue->is_boolean()",
		"EnqueueRelativePointerCapture(src, activeValue->get<bool>())" }),
		"relative-pointer capture edges from the browser transport must enter the main-thread request queue");
	Check(ContainsInOrder(viewRequestQueueSource, {
		"void OSFUI::ViewRequestQueue::EnqueueRelativePointer",
		"std::lock_guard<std::mutex> lock(m_mutex)",
		"m_relativePointer.push_back" }),
		"relative-pointer ownership edges must cross browser threads through the locked runtime queue");
	const auto relativeRequests = FunctionBody(runtimeSource, "void Runtime::ApplyRelativePointerRequests(");
	Check(ContainsInOrder(relativeRequests, {
		"if (!request.active)",
		"EndRelativePointerCapture(request.view)",
		"const auto active = _presentation.ActiveMenu()",
		"!IsInputCaptured() || !active || *active != request.view",
		"pointer-capture-forbidden",
		"BeginRelativePointerCapture(request.view)",
		"pointer-capture-unavailable" }),
		"the main-thread request drain must validate the live view owner before beginning capture and end it idempotently");
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
		"const auto active = _presentation.ActiveMenu()",
		"const bool desiredCapture = _presentation.DesiredCapture()",
		"*active != _relativePointerView",
		"CancelRelativePointerCapture()" }),
		"menu close, hide, and ownership switches must cancel relative-pointer capture through presentation policy");
	Check(ContainsInOrder(applyPolicy, {
		"const std::string shown = (visible && active) ? *active : std::string()",
		"shown != _lastShownView",
		"const std::string previous = _lastShownView",
		"_lastShownView = shown",
		"DispatchViewLifecycle(previous, API::Views::ViewLifecyclePhase::kHidden)",
		"DispatchViewLifecycle(shown, API::Views::ViewLifecyclePhase::kShown)" }),
		"native lifecycle hidden/shown callbacks must share the authoritative logical-menu transition and preserve replacement order");

	const auto relativeDrain = FunctionBody(runtimeSource, "void Runtime::DrainRelativePointerCapture()");
	Check(ContainsInOrder(relativeDrain, {
		"_relativePointerDx.exchange",
		"_relativePointerDy.exchange",
		"_relativePointerWheel.exchange",
		"RelativePointerPhase::kUpdate",
		"RelativePointerStop::kEnd",
		"RelativePointerPhase::kEnd",
		"RelativePointerStop::kCancel",
		"RelativePointerPhase::kCancel" }),
		"the main-frame drain must coalesce raw deltas before delivering one terminal edge");
	const auto frameTick = FunctionBody(runtimeFrameSource, "void Runtime::Tick(double a_deltaSeconds)");
	Check(ContainsInOrder(frameTick, {
		"ProcessBackendQueues",
		"ApplyPresentationRequests",
		"ProcessRendererFrame",
		"DrainRelativePointerCapture" }),
		"relative-pointer callbacks must drain once on the game main thread after browser ownership edges are processed");
	Check(ContainsInOrder(FunctionBody(runtimeSource, "void Runtime::ApplyPresentationRequests("), {
		"ApplyViewPresentationPolicy()",
		"ApplyRelativePointerRequests(a_work.relativePointer)" }),
		"relative-pointer ownership edges must be applied on the main thread after authoritative presentation changes");
	Check(ContainsInOrder(frameTick, {
		"ProcessRendererFrame",
		"DrainRelativePointerCapture",
		"if (!_lastShownView.empty())",
		"DispatchViewLifecycle(_lastShownView, API::Views::ViewLifecyclePhase::kFrame)" }) &&
		Count(frameTick, "ViewLifecyclePhase::kFrame") == 1,
		"the exact logically shown menu must receive one native lifecycle frame callback at the end of each game main tick");
	Check(ContainsInOrder(FunctionBody(overlayInputSource, "void RouteRawMouse(HWND a_hwnd, LPARAM a_lparam)"), {
		"MOUSE_MOVE_ABSOLUTE",
		"OnGameWindowMouseRelative",
		"g_hasLastAbsoluteClient",
		"OnGameWindowMouseRelative",
		"pt.x - g_lastAbsoluteClient.x",
		"OnGameWindowMouseAbsolute",
		"RI_MOUSE_LEFT_BUTTON_UP",
		"OnGameWindowMouseButton(0, false)" }),
		"WM_INPUT must accumulate both relative and absolute-device motion while preserving ordinary DOM pointer movement and the mouse-up terminal edge");
	const auto hostRawMouse = FunctionBody(hostSource, "void AccumulateRawMouse(LPARAM a_lparam)");
	Check(ContainsInOrder(hostRawMouse, {
		"GetRawInputData",
		"RI_MOUSE_WHEEL",
		"SendMouseInput",
		"relativePointerWheel += delta",
		"MOUSE_MOVE_ABSOLUTE",
		"relativePointerDx += mouse.lLastX",
		"relativePointerDy += mouse.lLastY" }),
		"the focused browser host must preserve WebView wheel delivery and forward physical relative motion for the admitted capture owner");
	Check(ContainsInOrder(FunctionBody(hostSource, "void FlushRelativePointer()"), {
		"msg::RelativePointer",
		"relativePointerDx = 0",
		"relativePointerDy = 0",
		"relativePointerWheel = 0",
		"Send(msg::ToJson(message))" }),
		"browser-host raw input must cross the pipe as one bounded message-pump batch");
	const auto hostRelativePointer = FunctionBody(runtimeSource,
		"void Runtime::OnBrowserHostRelativePointer(");
	Check(ContainsInOrder(hostRelativePointer, {
		"_relativePointerActive.load",
		"RelativePointerOwnerToken(a_viewId)",
		"_relativePointerHostInput.exchange(true",
		"_relativePointerDx.store(0.0f",
		"_relativePointerDx.fetch_add",
		"_relativePointerDy.fetch_add",
		"_relativePointerWheel.fetch_add" }),
		"the host pipe reader must validate the active owner and atomically replace the game-window fallback before accumulating motion");
	Check(ContainsInOrder(FunctionBody(runtimeSource,
		"bool Runtime::BeginRelativePointerCapture(std::string_view a_viewId)"), {
		"DispatchRelativePointer",
		"SetRelativePointerCapture(_relativePointerView, true)" }) &&
		ContainsInOrder(FunctionBody(runtimeSource, "void Runtime::FinishRelativePointerCapture("), {
			"SetRelativePointerCapture(_relativePointerView, false)",
			"_relativePointerDx.exchange" }),
		"successful capture begin/end edges must arm and disarm browser-host raw input around the final main-thread drain");
	Check(ContainsInOrder(applyPolicy, {
		"const auto layers = _presentation.DesiredLayers()",
		"SetViewOrder(layer.id, layer.z)",
		"if (!layer.hidden)",
		"SetViewHidden(layer.id, false)",
		"if (layer.hidden)",
		"SetViewHidden(layer.id, true)" }),
		"menu transitions must show the incoming layer before hiding the outgoing layer");
	Check(ContainsInOrder(applyPolicy, {
		"visible && !wasVisible",
		"m_viewReveal.Arm()" }),
		"a cold or reopened presentation must arm the fresh-frame reveal gate");
	Check(ContainsInOrder(applyPolicy, {
		"if (!visible)",
		"m_viewReveal.Cancel()" }),
		"closing during a held reveal must cancel the reveal gate");

	const auto reconcileFrame = FunctionBody(runtimeFrameSource,
		"void Runtime::ReconcileFrameState(double a_deltaSeconds)");
	Check(ContainsInOrder(reconcileFrame, {
		"ReconcileFocusMenu()",
		"ReconcileNativeFocus()" }),
		"native browser focus must be reconciled only after the engine FocusMenu state");
	const auto reconcileNativeFocus = FunctionBody(runtimeSource,
		"void Runtime::ReconcileNativeFocus()");
	Check(reconcileNativeFocus.find("FocusMenu::IsOpenInEngine()") != std::string::npos &&
		reconcileNativeFocus.find("_nativeFocusRefreshRequested.exchange(false)") !=
			std::string::npos,
		"native focus grants must wait for FocusMenu admission and accept event-driven refreshes");
	Check(ContainsInOrder(overlayInputSource, {
		"case kRestoreGameFocusMessage:",
		"if (!runtime.IsInputCaptured())",
		"::SetActiveWindow(a_hwnd)",
		"::SetFocus(a_hwnd)",
		"runtime.NotifyGameWindowFocused()",
		"case WM_SETFOCUS:" }),
		"queued game-focus restores must not steal focus from a newly active capture");

	const auto setInputTarget = FunctionBody(rendererSource,
		"void WebView2HostWebRenderer::SetInputTargetView(std::string_view a_id)");
	Check(ContainsInOrder(setInputTarget, {
		"focusRequested.load()",
		"focusEpoch.fetch_add(1)",
		"msg::Focus" }),
		"changing the active view while focused must create a new acknowledged focus epoch");
	const auto setNativeFocus = FunctionBody(rendererSource,
		"void WebView2HostWebRenderer::SetNativeFocus(bool a_focused)");
	Check(ContainsInOrder(setNativeFocus, {
		"focusRequested.store(a_focused)",
		"focusEpoch.fetch_add(1)",
		".focused = a_focused, .epoch = epoch, .view = target" }),
		"every native focus transition must carry a monotonic epoch and explicit target");
	const auto handleFocus = FunctionBody(hostSource, "void HandleFocus(const json& a_msg)");
	Check(ContainsInOrder(handleFocus, {
		"request.epoch < focusEpoch",
		"stale focus request ignored",
		"focusEpoch = request.epoch",
		"RequestInputFocus(\"focus request\")" }),
		"the browser host must reject stale focus requests before applying ownership");
	Check(hostSource.find("add_LostFocus") != std::string::npos &&
		hostSource.find("msg::FocusState") != std::string::npos &&
		hostSource.find("MoveFocus failed") != std::string::npos,
		"the host must report actual focus changes and surface MoveFocus failures");
	const auto releaseInputFocus = FunctionBody(hostSource,
		"void ReleaseInputFocus(std::string_view a_reason)");
	Check(ContainsInOrder(releaseInputFocus, {
		"::SetFocus(hostWindow)",
		"::SetFocus(nullptr)",
		"PublishFocusState()",
		"QueueGameFocusRestore()" }) &&
		hostSource.find("ReleaseInputFocus(\"unexpected GotFocus\")") != std::string::npos &&
		hostSource.find("ReleaseInputFocus(\"view show\")") != std::string::npos,
		"focus revocation must synchronously clear Chromium before restoring Starfield focus");
	Check(rendererSource.find("kRepairDelaySeconds = 1.0") != std::string::npos &&
		rendererSource.find("focusAckEpoch >= epoch") != std::string::npos,
		"the focus watchdog must wait for a persistent acknowledgement mismatch");

	const auto setHidden = FunctionBody(rendererSource,
		"void WebView2HostWebRenderer::SetViewHidden(std::string_view a_viewId, bool a_hidden)");
	Check(ContainsInOrder(setHidden, {
		"view->hidden == a_hidden",
		"wasHidden",
		"RecomputeAllHidden()",
		"wasHidden && !a_hidden",
		"++_impl->presentationEpoch",
		"_impl->haveFrame = false",
		".presentationEpoch = presentation" }),
		"every newly shown view must advance the presentation epoch and invalidate cached pixels");

	const auto onFrame = FunctionBody(rendererSource, "void OnFrameMessage(const msg::Frame& a_msg)");
	Check(ContainsInOrder(onFrame, {
		"allHidden || presentation != presentationEpoch",
		"haveFrame = false",
		"ackNew = true" }),
		"all-hidden and wrong-epoch frames must be rejected and acknowledged");
	Check(onFrame.find("ackSlot = frameSlot") != std::string::npos &&
		onFrame.find(".slot = ackSlot") != std::string::npos &&
		onFrame.find(".slot = slot") != std::string::npos,
		"discard acknowledgements must release only the exact superseded ring slot");
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
	Check(ContainsInOrder(submitFrame, {
		"if (decision.submitFrame && frame)",
		"_compositor->Submit(*frame)",
		"observation->outputSizeKnown && observation->matchesExpectedSize",
		"FinishColdOpenTiming(*active)",
		"if (decision.reveal)",
		"_compositor->SetVisible(true)" }),
		"cold-open timing must finish on the first presentable submitted frame, including visible menu-to-menu transitions");
	const auto finishColdOpen = FunctionBody(runtimeSource,
		"void Runtime::FinishColdOpenTiming(std::string_view a_viewId)");
	Check(ContainsInOrder(finishColdOpen, {
		"timing.requestedAt, revealedAt",
		"timing.requestedAt, *timing.instantiatedAt",
		"*timing.instantiatedAt, *timing.loadedAt",
		"*timing.loadedAt, revealedAt" }) &&
		finishColdOpen.find("cold-open timing") != std::string::npos &&
		finishColdOpen.find("request->instantiate") != std::string::npos,
		"cold-open diagnostics must summarize total, dispatch, load and presentable-frame timing in one line");
	const auto finishHiddenPrewarm = FunctionBody(runtimeSource,
		"void Runtime::FinishHiddenPrewarmTiming(std::string_view a_viewId");
	Check(ContainsInOrder(finishHiddenPrewarm, {
		"if (_coldOpenTiming && _coldOpenTiming->viewId == a_viewId)",
		"_hiddenPrewarmTiming.reset()",
		"return",
		"hidden-prewarm timing",
		"timing.requestedAt, a_loadedAt",
		"timing.requestedAt, *timing.instantiatedAt",
		"*timing.instantiatedAt, a_loadedAt" }),
		"hidden prewarm must emit one summary, suppressed when a player-visible cold open owns the timing line");
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
		"sharedRing.consumeFences[ringSlot]",
		"DrawInstanced(3, 1, 0, 0)" }),
		"each UI target must draw the cached frame and track completion on that slot's own fence");
	const auto trackConsume = FunctionBody(compositorSource,
		"[[nodiscard]] bool Track(");
	Check(ContainsInOrder(trackConsume, {
		"pending.begin(), pending.end()",
		"pending.push_back",
		"a_fence->AddRef()",
		"pendingCount.store(pending.size()" }) &&
		trackConsume.find("pendingSize == pending.size()") == std::string::npos,
		"consume tracking must grow beyond its reserved fast-path capacity instead of disabling overlay draws");
	Check(trackConsume.find("a_pending.list == a_list && a_pending.fence == a_fence") != std::string::npos &&
		trackConsume.find("tracked.fence = a_fence") == std::string::npos,
		"one command list must retain a separate consume record for every ring-slot fence it reads");
	const auto executeConsumes = FunctionBody(compositorSource,
		"void OnCommandListsExecuted(");
	Check(ContainsInOrder(executeConsumes, {
		"const auto wasExecuted",
		"std::scoped_lock lock(mutex)",
		"while (true)",
		"serial = (std::max)(serial, tracked.serial)",
		"a_queue->Signal(fence, serial)",
		"pending.pop_back()" }) &&
		executeConsumes.find("std::array<PendingConsume") == std::string::npos,
		"consume completion must remain allocation-free and handle every tracked list in one submission");
	Check(compositorSource.find("pending.reserve(kReservedEntries)") != std::string::npos &&
		compositorSource.find("std::vector<PendingConsume> pending") != std::string::npos &&
		compositorSource.find("consume tracker capacity") == std::string::npos,
		"consume tracking must reserve the normal working set while retaining dynamic overflow");
	const auto setDescriptorHeaps = FunctionBody(uiPassSource,
		"void STDMETHODCALLTYPE SetDescriptorHeapsThunk(");
	Check(setDescriptorHeaps.find("else if (!tl_inOverlayDraw)") != std::string::npos &&
		setDescriptorHeaps.find("TrackingHeaps()") == std::string::npos,
		"descriptor-heap tracking must remain active outside the narrow handoff window");
	const auto beginUiPass = FunctionBody(uiPassSource,
		"void* BeginThunk(");
	Check(beginUiPass.find("tl_heapList = nullptr") == std::string::npos &&
		beginUiPass.find("tl_heapCount = 0") == std::string::npos,
		"Scaleform Begin must preserve an inherited descriptor-heap binding");
	const auto endUiPass = FunctionBody(uiPassSource,
		"void* EndThunk(");
	const auto compositeUiPass = FunctionBody(uiPassSource,
		"void* CompositeThunk(");
	Check(beginUiPass.find("tl_handoffWindow.Cancel()") != std::string::npos &&
		beginUiPass.find("tl_handoffWindow.Begin()") == std::string::npos &&
		endUiPass.find("tl_handoffWindow.End()") == std::string::npos &&
		ContainsInOrder(compositeUiPass, {
			"tl_handoffWindow.Begin()",
			"original ? original(a_this, a_ctx, a_io, a_r9) : nullptr",
			"tl_handoffWindow.End()" }),
		"the overlay handoff must open after ScaleformComposite instead of before Starfield's fixed-aspect transform");
	const auto recordAtHandoff = FunctionBody(uiPassSource,
		"const bool a_fgTarget");
	Check(ContainsInOrder(recordAtHandoff, {
		"const bool heapKnown",
		"if (!heapKnown)",
		"return",
		"RecordOverlayIntoRenderTarget",
		"original(a_list, engineHeapCount, engineHeaps)" }),
		"overlay recording must fail closed without restorable engine heaps and restore known heaps after drawing");
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

	const auto hostLog = FunctionBody(hostSource, "void Log(int a_level, const std::string& a_text)");
	Check(hostLog.find("::GetLocalTime(&localTime)") != std::string::npos &&
		hostLog.find("localTime.wMilliseconds") != std::string::npos &&
		hostLog.find("system_clock::now()") == std::string::npos,
		"browser-host file timestamps must use local wall time with millisecond precision like the SFSE log");
	const auto runHost = FunctionBody(hostSource, "int RunHost(const HostOptions& a_options)");
	Check(ContainsInOrder(runHost, {
		"std::format(L\"Local\\\\osfui-wv2-host-{}\", a_options.gamePid)",
		"::CreateMutexW",
		"ERROR_ALREADY_EXISTS" }),
		"production must admit at most one browser host for each Starfield process");
	Check(ContainsInOrder(runHost, {
		"app.pipe.ServerProcessId()",
		"*serverPid != a_options.gamePid",
		"app.gameProcess = ::OpenProcess(",
		"PROCESS_DUP_HANDLE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION" }),
		"the browser host must authenticate and retain a waitable handle to its owning Starfield process");

	const auto hostRun = FunctionBody(hostSource, "int Run()");
	Check(ContainsInOrder(hostRun, {
		"const HANDLE waits[2] = { wakeEvent, gameProcess }",
		"::MsgWaitForMultipleObjectsEx(",
		"if (wait == WAIT_OBJECT_0 + 1)",
		"break" }),
		"the browser host must leave its message loop when the owning Starfield process exits");
	Check(ContainsInOrder(hostRun, {
		"if (pipeDead.load())",
		"captureGameExit(1000)",
		"break" }),
		"a dead game pipe must also end the browser-host process");
	Check(hostSource.find("PromptCrashReport") == std::string::npos,
		"browser-host teardown must not block on the removed post-crash reporting prompt");

	const auto stopRenderer = FunctionBody(rendererSource, "void Stop(bool a_force = false)");
	Check(ContainsInOrder(stopRenderer, {
		"RequestShutdown()",
		"pipe.Close()",
		"worker.join()",
		"TakeBrowserHostProcess()",
		"::WaitForSingleObject(browserHostProcess, graceMs)",
		"::TerminateProcess(browserHostProcess, 9)" }),
		"renderer teardown must terminate its verified browser host when graceful shutdown times out");

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
			"EnqueueOpenView(manifest.id)" }) &&
		startup.find("InstantiateView") == std::string::npos &&
		startup.find("_presentation.Open") == std::string::npos,
		"startup must queue eligible HUD opens so native view-open preflights can run before instantiation");
	const auto registrations = FunctionBody(runtimeSource,
		"void Runtime::DrainViewRegistrations(std::vector<std::string> a_ids)");
	Check(ContainsInOrder(registrations, {
		"if (m->openOnStart)",
		"BeginViewOpen(id, \"via plugin RegisterView openOnStart\")" }) &&
		registrations.find("InstantiateView(*m") == std::string::npos,
		"plugin openOnStart must use the same view-open preflight path");
	Check(ContainsInOrder(postDataIntegration, {
		"BeginHiddenPrewarmTiming(settings->id)",
		"InstantiateView(*settings, \"for hidden startup prewarm\")" }) &&
		postDataIntegration.find("BeginViewOpen") == std::string::npos &&
		postDataIntegration.find("RunViewOpenPreflight") == std::string::npos,
		"hidden prewarming must instantiate directly without running the view-open preflight");

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
	const auto onController = FunctionBody(hostSource,
		"HRESULT OnController(View& a_view");
	Check(onController.find(
		"SetVirtualHostNameToFolderMapping(kViewHost.data(), viewsRoot.c_str()") !=
		std::string::npos &&
		onController.find("modRoot") == std::string::npos,
		"every WebView must map the one OSF UI host to the complete prepared views root");
	const auto installNetworkGuard = FunctionBody(hostSource,
		"HRESULT InstallNetworkGuard(View& a_view)");
	Check(ContainsInOrder(installNetworkGuard, {
		"IsAllowedViewResourceUri(uri, kViewHost)",
		"CreateWebResourceResponse(nullptr, 403",
		"a_args->put_Response(response.Get())" }),
		"the shared origin must remain the only allowed network resource host");
	Check(hostSource.find("VirtualHostForMod") == std::string::npos &&
		hostSource.find("BCrypt") == std::string::npos &&
		hostSource.find("LegacySharedAsset") == std::string::npos &&
		hostSource.find("virtualHost") == std::string::npos &&
		hostGraphics.find("VirtualHostForMod") == std::string::npos &&
		hostGraphics.find("virtualHost") == std::string::npos,
		"the browser host must not retain per-mod or legacy asset-origin machinery");
	Check(ContainsInOrder(hostSource, {
		"L\"https://\" + std::wstring(kViewHost)",
		"view->modId",
		"view->viewName" }) &&
		hostSource.find("source, kViewHost, view->modId, view->viewName") != std::string::npos &&
		hostSource.find("uri, kViewHost, a_view.modId, a_view.viewName") != std::string::npos,
		"documents and native-bound messages must stay scoped to /<mod>/<view> on the shared host");
	Check(hostSource.find("SHCreateStreamOnFileEx") == std::string::npos,
		"virtual-host resources must not rely on WebResourceRequested, which WebView2 does not raise for mapped files");
	const auto resolveMappedViews = FunctionBody(rendererSource,
		"bool ResolveMappedViewsRoot()");
	Check(resolveMappedViews.find("if (!::GetModuleHandleW(L\"usvfs_x64.dll\")) return true") !=
			std::string::npos &&
		resolveMappedViews.find("MaterializeSharedAssets") == std::string::npos,
		"only USVFS launches should mirror the already-complete views root");
	const auto refreshViewFiles = FunctionBody(rendererSource,
		"bool RefreshViewFiles(std::string_view a_viewId)");
	Check(refreshViewFiles.find("DevViewFiles::SyncTree(source, destination, error)") !=
			std::string::npos &&
		refreshViewFiles.find("viewsRoot / \"shared\"") == std::string::npos &&
		refreshViewFiles.find("MaterializeSharedAssets") == std::string::npos,
		"developer reload should mirror only the changed mod subtree without rebuilding shared projections");
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

	Check(Count(applyRequests, "CancelPendingOpen()") >= 3 &&
		Count(applyRequests, "CancelPendingOpen(r.view)") >= 1,
		"all four queued close paths must retain pending-open cancellation");

	if (failures == 0) {
		std::cout << "runtime_lifecycle_contract_tests: ok\n";
	}
	return failures;
}
