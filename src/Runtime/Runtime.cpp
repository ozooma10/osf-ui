#include "Runtime/Runtime.h"

#include <cmath>
#include <limits>

#include "RE/C/Calendar.h"

#include "API/BridgeApi.h"
#include "API/PapyrusApi.h"
#include "Compat/V1/Papyrus.h"
#include "Composite/D3D12Compositor.h"
#include "Composite/UiPassSeam.h"
#include "Core/Log.h"
#include "Core/Version.h"
#include "Input/ControlLayer.h"
#include "Input/EngineInput.h"
#include "Input/FocusMenu.h"
#include "Input/FreeCursor.h"
#include "Input/HardwareCursor.h"
#include "Input/KeyNames.h"
#include "Input/MainThreadMenuPump.h"
#include "Input/MenuMode.h"
#include "Input/MenuEventSink.h"
#include "Input/OverlayInputHook.h"
#include "Input/PauseMenuEntry.h"
#include "Input/SimPause.h"
#include "Input/UiLayoutGuard.h"
#include "Input/XInputPoller.h"
#include "Core/Paths.h"
#include "Platform/WindowsPlatform.h"
#include "Core/Json.h"
#include "Core/Ids.h"
#include "API/PapyrusCall.h"
#include "Render/WebView2HostWebRenderer.h"

namespace OSFUI
{
	namespace
	{
		constexpr std::string_view kHandoffViewId{ "osfui/handoff" };
		// Mod Settings: pinned alongside the handoff (the pause-menu
		// entry and toggle key both land here) and the only source allowed to
		// change player view policy.
		constexpr std::string_view kSettingsViewId{ "osfui/settings" };
		constexpr double           kHandoffDelaySeconds{ 0.15 };
		constexpr double           kReadySignalTimeoutSeconds{ 15.0 };
		constexpr KeyCode          kVkF12{ 0x7B };
		// Latched when an armed capture consumed a press whose scan code could
		// not be recovered (SendInput-synthesized input with no scan and no
		// platform mapping). Outside the 8-bit DIK range, so KeyName() returns
		// "" and DrainKeyCapture answers with a cancel instead of staying armed.
		constexpr ScanCode         kUnnameableScan{ 0xFFFF };
		// Capture-reserved physical keys (DrainKeyCapture): Esc cancels a rebind
		// by contract, and a Win keyup outside exclusive fullscreen opens the
		// Start menu, so binding either would be a foot-gun.
		constexpr ScanCode         kScanEscape{ 0x01 };
		constexpr ScanCode         kScanLWin{ 0xDB };
		constexpr ScanCode         kScanRWin{ 0xDC };

	}

	Runtime& Runtime::Get()
	{
		// ExitProcess stops worker threads before DLL static destruction. The
		// runtime owns those workers, so destroying it from process detach can
		// never be made safe; the OS reclaims its in-process resources and the
		// detached browser host independently watches the game process handle.
		static Runtime* const instance = new Runtime;
		return *instance;
	}

	bool Runtime::LoadRuntimeConfig()
	{
		if(!Paths::Initialize()) {
			return false;
		}

		_config = Config::Load(Paths::ConfigFile());
		Log::SetDevMode(_config.devMode);
		return true;
	}

	void Runtime::LoadStartupContent()
	{
		const auto documents = Platform::GetDocumentsPath();
		const auto starfieldDir = documents.empty() ? std::filesystem::path{} : documents / "My Games" / "Starfield";

		_localization.Load(Paths::DataDir() / "l10n", LocalizationService::DetectGameLocale(starfieldDir));

		PauseMenuEntry::Configure(
			_localization.Resolve("osfui", "chrome.pauseMenuEntry", _config.pauseMenuEntryLabel),
			_config.pauseMenuEntryView
		);

		PauseMenuEntry::SetEnabled(_config.pauseMenuEntry);

		_views.DiscoverAll(Paths::ViewsDir());
		_viewPolicy.Load(Paths::DataDir() / "state" / "view-policy.json");

		std::vector<std::string> discoveredViewIds;
		discoveredViewIds.reserve(_views.All().size());

		for (const auto& manifest : _views.All()) {
			discoveredViewIds.push_back(manifest.id);
		}

		API::BridgeApi::Get().SetViewCatalog(discoveredViewIds);
	}

	bool Runtime::InitializeRenderer()
	{
		_renderer = std::make_unique<WebView2HostWebRenderer>();

		const auto* view = _views.Find(_config.view);
		const auto initialWidth = view ? view->width : kDefaultViewWidth;
		const auto initialHeight = view ? view->height : kDefaultViewHeight;

		_viewWidth.store(initialWidth);
		_viewHeight.store(initialHeight);
		_cursorX = initialWidth * 0.5f;
		_cursorY = initialHeight * 0.5f;

		RendererConfig rendererConfig{
			.width = initialWidth,
			.height = initialHeight,
			.devMode = _config.devMode,
			.dataDir = Paths::DataDir(),
		};

		if (!_renderer->Initialize(rendererConfig)) {
			REX::ERROR("Runtime: WebView2 renderer failed to initialize");
			return false;
		}

		REX::INFO("Runtime: renderer = {}", _renderer->Name());
		return true;
	}

	void Runtime::WireRendererLifecycleCallbacks()
	{
		_renderer->SetLoadHandler([this](const IWebRenderer::LoadEvent& a_e) {
			OnViewLoad(a_e.viewId, a_e.failed, a_e.url, a_e.description, a_e.errorCode);
		});

		_renderer->SetFailureHandler([this](const IWebRenderer::FailureEvent& a_e) {
			OnRendererFailure(a_e);
		});

		_renderer->SetHealthHandler([this](const IWebRenderer::HealthEvent& a_e) {
			_runtimeHealth.OnRendererHealth(a_e);
		});

		_renderer->SetCursorChangeHandler([](CursorShape a_shape) {
			HardwareCursor::SetShape(a_shape);
		});
	}

	bool Runtime::InitializeCompositor()
	{
		_compositor = std::make_unique<D3D12Compositor>();
		if (!_compositor->Initialize()) {
			REX::ERROR("Runtime: D3D12 compositor failed to initialize");
			return false;
		}
		REX::INFO("Runtime: compositor = {}", _compositor->Name());
		return true;
	}

	void Runtime::WireRenderPipeline()
	{
		_renderer->SetSharedRingHandler([this](const SharedRingDesc& a_desc) {
			if (_compositor) {
				_compositor->SetSharedRing(a_desc);
			}
		});

		_compositor->SetOutputResizeCallback([this](std::uint32_t a_w, std::uint32_t a_h) {
			OnOutputResized(a_w, a_h);
		});
	}

	void Runtime::InitializeFeatureModules()
	{
		// Settings: schemas ship read-only under <data>/settings/*.json; values persist per-mod under <data>/settings/values 
		const auto schemaDir = Paths::DataDir() / "settings";
		const auto valuesDir = schemaDir / "values";
		auto settings = std::make_unique<SettingsModule>(schemaDir, valuesDir,
			[this](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
				OnSettingChanged(a_mod, a_key, a_value);
			},
			// v1 -> v2 values migration: pre-2.x key names were VK-anchored;
			// re-anchor each one to the physical key that VK sits on under the
			// layout active right now. Every failed step keeps the spelling —
			// on US layouts the whole chain is an identity.
			[](const std::string& a_name) -> std::string {
				const auto vk = Legacy::ResolveKeyNameVk(a_name);
				if (vk == 0) {
					return a_name;
				}
				const auto scan = Platform::VkToDirectInputScan(vk);
				if (scan == 0) {
					return a_name;
				}
				auto name = KeyName(static_cast<ScanCode>(scan));
				return name.empty() ? a_name : name;
			});
		_settings = settings.get();  // core needs schema facts (e.g. key-capture gating)
		_settings->Store().SetTextResolver([this](std::string_view a_mod, std::string_view a_address, std::string_view a_english) {
			return _localization.Resolve(a_mod, a_address, a_english);
		});

		// Native ABI feed: every committed value — including the
		// OnStart NotifyAll replay below and the per-mod replay after an
		// incremental RegisterSchema — lands in the any-thread mirror the C ABI
		// typed getters read, then queues for SubscribeSettings consumers (drained
		// on the main thread by BridgeApi::PumpMainThread). Mirror first: a
		// subscribe replay snapshots the mirror, so it must never lag the queued
		// event. Registry shape changes rebuild the mirror from the store document
		// so a removed mod's values stop resolving.
		auto& store = _settings->Store();
		store.AddChangeListener([](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
			auto& api = API::BridgeApi::Get();
			api.Mirror().Update(a_mod, a_key, a_value);
			api.Subscriptions().OnChanged(a_mod, a_key, a_value);
			// Papyrus change callbacks, after the mirror
			// update: the dispatched script call reads current values through the
			// mirror-backed getters, so the mirror must never lag it.
			API::Papyrus::OnSettingChanged(a_mod, a_key);
		});
		store.AddRegistryListener([this] {
			if (_settings) {  // teardown guard (_settings nulls before modules die)
				API::BridgeApi::Get().Mirror().Rebuild(_settings->Store().DataView());
			}
		});

		// HotkeyService: every key-typed setting is a dispatchable mod-hotkey
		// binding. The registry rebuilds on any key-typed commit (web, ABI or
		// reset) and on registry shape change; the store's conflict grouping shares
		// this key-name resolution, so the store stays input-agnostic. Suppression
		// reads the same capture state OnGameWindowKey consults, so a press while the
		// user types in a settings field or mid-rebind cannot fire a hotkey.
		store.SetKeyNameResolver(ResolveKeyName);

		// Game bindings are not loaded here: the Mod Settings-owned
		// osfui.vanillaKeyConflicts setting's OnStart replay drives
		// ApplyGameBindingConflictWarnings with the persisted value (default
		// on → loads then; off → never pays the parse).

		_hotkeys.SetSuppression([this] { return IsInputCaptured() || _captureArmed.load(); });
		store.AddChangeListener([this](std::string_view a_mod, std::string_view a_key, const nlohmann::json&) {
			if (_settings && _settings->Store().GetSettingType(a_mod, a_key) == "key") {
				_hotkeys.Rebuild(_settings->Store());
			}
		});
		store.AddRegistryListener([this] {
			if (_settings) {
				_hotkeys.Rebuild(_settings->Store());
			}
		});
		_hotkeys.Rebuild(store);  // LoadAll already ran in the module's constructor

		// First keycap-label build (localized display for the current layout);
		// later switches re-derive via WM_INPUTLANGCHANGE / locale / capture-arm.
		RefreshKeyboardLabels("startup");

		_modules.push_back(std::move(settings));

		// System Health (introduced in web bridge protocol 1.4): a session-scoped registry every
		// subsystem reports durable, actionable conditions to. Deliberately
		// LAST, so a producer that fires during another module's OnStart finds
		// the registry already constructed.
		auto healthRegistry = std::make_unique<HealthRegistry>();
		_healthRegistry = healthRegistry.get();
		_modules.push_back(std::move(healthRegistry));

		REX::INFO("Runtime: {} UI module(s) loaded", _modules.size());

		for (const auto& module : _modules) {
			module->OnStart();
		}
	}

	void Runtime::InitializeBridge()
	{
		_bridge = std::make_unique<MessageBridge>([this](std::string_view a_viewId, std::string_view a_json) {
			if (_renderer) {
				_renderer->SendMessageToWeb(a_viewId, a_json);
			}
		});
		
		_bridge->SetHelloHook([this](std::string_view a_viewId) { OnViewGreeted(a_viewId); });

		_bridge->SetProtocolFaultSink([this](std::string_view a_viewId, std::string_view a_code, std::string_view a_message, 
			const nlohmann::json& a_detail, bool a_viewFault) {
			OnProtocolFault(a_viewId, a_code, a_message, a_detail, a_viewFault);
		});

		RegisterPlatformEndpoints(*_bridge);

		for(const auto& module : _modules) {
			module->RegisterEndpoints(*_bridge);
		}

		_renderer->SetWebMessageHandler([this](std::string_view a_viewId, std::string_view a_json) {
			if (_bridge) {
				_bridge->HandleWebMessage(a_viewId, a_json);
			}
		});
	}

    void Runtime::InitializeStartupViews()
    {
		_pinnedViews.clear();
		for (const auto id : { kHandoffViewId, kSettingsViewId }) {
			if (_views.Find(id)) {
				_pinnedViews.emplace(id);
			}
		}

		std::size_t instantiated = 0;
		const auto instantiatePinned = [this, &instantiated](std::string_view a_id, std::string_view a_reason) {
			const auto* manifest = _views.Find(a_id);
			const bool wasInstantiated = _presentation.IsInstantiated(a_id);
			if (!manifest || !InstantiateView(*manifest, a_reason)) {
				return;
			}
			if (!wasInstantiated) {
				++instantiated;
			}
			// prewarm view so first open is immediateish
			_renderer->PrewarmView(a_id);
		};
		instantiatePinned(kHandoffViewId, "as the pinned first-load handoff");
		instantiatePinned(kSettingsViewId, "as the pinned Mod Settings view");

		for (const auto& manifest : _views.All()) {
			if (manifest.kind != ViewKind::Hud || _pinnedViews.contains(manifest.id)) {
				continue;
			}
			if (!HudAutoStartEligible(manifest)) {
				if (_viewPolicy.HasHudOverride(manifest.id)) {
					REX::DEBUG("Runtime: HUD '{}' has an auto-start override but is not eligible (catalog-hidden via hub:false, or debugOnly without developer mode); ignored", manifest.id);
				}
				continue;
			}
			if (!_viewPolicy.HudAutoStart(manifest.id, manifest.openOnStart)) {
				continue;
			}
			if (InstantiateView(manifest, "for HUD auto-start")) {
				++instantiated;
				if (_presentation.IsInstantiated(manifest.id)) {
					_presentation.Open(manifest.id);
				}
			}
		}
		REX::INFO("Runtime: instantiated {} pinned/auto-start view(s); default menu = '{}'", instantiated, _config.view);
		if (!_views.Find(_config.view)) {
			REX::WARN("Runtime: default view '{}' was not discovered; the toggle key will have nothing to open", _config.view);
		}
    }

    void Runtime::ConfigureInputRouting()
    {
		const auto toggleKey = ResolveKeyName(_config.toggleKey);
		_toggleKey.store(toggleKey, std::memory_order_release);

		if (toggleKey != kInvalidScanCode) {
			REX::INFO("Runtime: toggleKey '{}' resolved to scan code {:#x}", _config.toggleKey, toggleKey);
		}

		_renderer->SetNativeAcceleratorHandler([this](std::uint32_t a_vkCode, std::uint32_t a_scanCode, bool a_down) {
			return OnNativeAcceleratorKey(a_vkCode, a_scanCode, a_down);
		});
    }

    bool Runtime::Initialize()
	{
		if (_initialized) {
			return true;
		}
		_rendererFailed = false;
		_rendererFailureLatched = false;
		_browserHostRecovery.Reset();

		if(!LoadRuntimeConfig()) {
			return false;
		}

		if (!_config.enabled) {
			REX::INFO("Runtime: disabled via config; nothing further will be initialized");
			return true;
		}

		LoadStartupContent();

		if(!InitializeRenderer()) {
			return false;
		}

		WireRendererLifecycleCallbacks();

		if(!InitializeCompositor()) {
			return false;
		}
		WireRenderPipeline();
		InitializeFeatureModules();
		InitializeBridge();
		InitializeStartupViews();
		ConfigureInputRouting();

		if (_config.devMode) {
			_devViewReload = std::make_unique<DevViewReloadWorker>(
				Paths::ViewsDir(), [this](std::string_view a_id) {
					return _renderer && _renderer->RefreshViewFiles(a_id);
				});
		}

		_initialized = true;
		// Push the initial policy derived from whatever is open (incl. nothing).
		ApplyViewPresentationPolicy();
		REX::INFO("Runtime: initialized (visible={})", _visible.load());

		return true;
	}

	bool Runtime::InstallOverlayDrawPath()
	{
		if (!_config.enabled || !_compositor) {
			return false;
		}
		const bool installed = UiPassSeam::Install();
		_overlayDrawAvailable.store(installed, std::memory_order_release);
		_compositor->SetSeamDrawMode(installed);
		if (!installed) {
			REX::ERROR("Runtime: the Scaleform UI seam could not be hooked — menu opens will be "
					   "refused this session so OSF UI cannot capture input without a draw path. "
					   "See the [UiPassSeam] lines above.");
		}
		return installed;
	}

	void Runtime::OnDataLoaded()
	{
		// Messaging callbacks are not serialized with the BSService-backed main-
		// thread tick. Never expose a partially initialized ControlMap snapshot to
		// an already-running tick; only publish a coalesced work notification here.
		_controlMapInit.Request();
	}

	void Runtime::OnPostDataLoaded()
	{
		_uiIntegrationInit.Request();
	}

	void Runtime::InitializeDataLoadedState()
	{
		REX::DEBUG("Runtime: consuming kPostDataLoad work on the main-thread tick");
		API::Papyrus::Install();
		_controlMap.Initialize();
		SyncLiveControlMapBindings();
		SyncLiveControlMapHealth();
		PublishPlatformState("keybindings");
		PublishPlatformState("input-context");
	}

	void Runtime::InitializePostDataLoadIntegration()
	{
		REX::DEBUG("Runtime: consuming kPostPostDataLoad work on the main-thread tick");
		if (!UiLayoutGuard::VerifyUiLayout()) {
			REX::ERROR("Runtime: UI layout guard failed; skipping ALL UI integration "
				"(menu events, FocusMenu and the WndProc hook stay uninstalled; capturing menus are unavailable)");
			return;
		}
		const bool menuEventsInstalled = MenuEventSink::Install();
		// Hook the main-loop UI update so PauseMenuEntry's Scaleform access runs
		// not only on the main thread, but specifically after active movies have
		// advanced and nothing else is inside the AS3 VM.
		MainThreadMenuPump::Install();
		const bool focusMenuRegistered = FocusMenu::Register();
		// The WndProc subclass is the only input path: it drives the toggle key
		// and consumes/routes keyboard and mouse while the overlay captures input.
		const bool inputInstalled = OverlayInputHook::Install();
		_captureIntegrationAvailable = menuEventsInstalled && focusMenuRegistered && inputInstalled;
		if (!_captureIntegrationAvailable) {
			REX::ERROR("Runtime: required input integration is unavailable; menus that capture input will be refused this session");
		}
	}

    void Runtime::ProcessLifecycleWork()
    {
		if(_controlMapInit.Take()) {
			InitializeDataLoadedState();
		}

		if(_uiIntegrationInit.Take()) {
			InitializePostDataLoadIntegration();
		}

		DriveBrowserHostRecovery();
    }

    void Runtime::ProcessControlMapUpdates()
    {
		if(_keyboardLayoutChanged.exchange(false)) {
			RefreshKeyboardLabels("input language change");

			if(_controlMap.RefreshLabels(false)) {
				SyncLiveControlMapBindings();
				PublishPlatformState("keybindings");
			} else if(_controlMap.Initialized() && !_controlMap.Available()) {
				SyncLiveControlMapBindings();
				SyncLiveControlMapHealth();
				PublishPlatformState("keybindings");
				PublishPlatformState("input-context");
			}
		}

		const auto changes = _controlMap.Pump();

		if(changes.keybindings) {
			SyncLiveControlMapBindings();
			SyncLiveControlMapHealth();
			PublishPlatformState("keybindings");
		}

		if(changes.engineInputContext) {
			SyncLiveControlMapHealth();
			PublishPlatformState("input-context");
		}
	}

    void Runtime::Tick(double a_deltaSeconds)
	{
		if (!_initialized) {
			return;
		}
		_uptime += a_deltaSeconds;

		ProcessLifecycleWork();

		DrainViewRegistrations();
		const auto presentationWork = TakePresentationRequests();
		PreparePresentationRequests(presentationWork);

		ProcessControlMapUpdates();

		DrainKeyCapture();
		DrainHotkeys();
		DrainSchemaOps();
		// Papyrus Set*/Reset operations go through the same validated
		// store path as every other writer. After DrainSchemaOps so a set against a
		// just-registered schema resolves this tick.
		if (_settings) {
			API::Papyrus::DrainSettingsOps(_settings->Store());
		}
		// Papyrus state and events reach the publishing mod's instantiated views before
		// PumpMainThread/Update flush the per-view outbound queues, so both land
		// in this tick's frame. No subscriber set: the target list is derived
		// fresh from the instantiated views each time, so there is nothing to prune
		// or go stale.
		if (_bridge) {
			// A game load reset the VM: drop retained PAPYRUS state, whose values
			// can hold session-scoped form identities. Native plugin state is
			// left alone — a plugin's HUD config has no such lifetime, and
			// wiping it on every load would be the bug.
			if (API::Papyrus::TakeSessionReset()) {
				_retainedState.ClearSessionScoped();
				Compat::V1::Papyrus::ClearPendingPushes();
			}
			// SetView* is RETAINED: it goes into the shared store first, so a
			// document that greets the bridge later is replayed the same value.
			// This is why a Papyrus-backed HUD survives F5 with no re-push
			// handshake in the script.
			API::Papyrus::DrainViewState([this](const API::Papyrus::ViewState& a_state) {
				_retainedState.Set(a_state.mod, a_state.key, a_state.value, /*sessionScoped*/ true);
				PublishModState(a_state.mod, a_state.key, a_state.value);
			});
			// Temporary 1.x PushToView/PushFormsToView adapter: transient by
			// contract, so emit data.push and never retain/replay it.
			Compat::V1::Papyrus::DrainPushes([this](const Compat::V1::Papyrus::Push& a_push) {
				const auto targets = InstantiatedViewsOfMod(a_push.mod);
				if (!targets.empty()) _bridge->Emit(targets, "data.push", a_push.payload);
			});
			// The native ABI's half of the same grid (SetViewState). Same store,
			// same replay — a plugin sets a value once and every fresh document
			// of its mod is handed it, exactly like Papyrus state. NOT
			// session-scoped: a plugin's state holds no form identities.
			for (auto& op : API::BridgeApi::Get().TakeViewStateOps()) {
				_retainedState.Set(op.mod, op.key, op.value, /*sessionScoped*/ false);
				PublishModState(op.mod, op.key, op.value);
			}
			// SendViewEvent is a one-shot happening: never retained, never
			// replayed. Encoding one as state would re-fire its effect on every
			// reload, which is exactly the bug the split exists to prevent.
			API::Papyrus::DrainViewEvents([this](const API::Papyrus::ViewEvent& a_event) {
				const auto targets = InstantiatedViewsOfMod(a_event.mod);
				if (targets.empty()) {
					REX::DEBUG("Runtime: SendViewEvent {}.{} had no instantiated '{}/...' view to deliver to",
						a_event.mod, a_event.name, a_event.mod);
					return;
				}
				_bridge->Emit(targets, std::format("{}.{}", a_event.mod, a_event.name),
					nlohmann::json{ { "args", a_event.args } });
			});
			API::Papyrus::DrainViewReplies([this](const API::Papyrus::ViewReply& reply) {
				if (reply.rejected) {
					_bridge->RejectTo(reply.deferToken, reply.code, reply.message);
				} else {
					_bridge->RespondTo(reply.deferToken, nlohmann::json{ { "value", reply.value } });
				}
			});
		}
		// Expire deferred requests past the OSF UI runtime deadline with `no-response`,
		// before the pump below, so an endpoint handler that stopped answering frees the
		// caller's in-flight capacity this tick rather than next.
		if (_bridge) {
			_bridge->Tick();
		}
		// Apply the native plugin API's queued ops (endpoint (re)registration +
		// off-thread sends) on the main thread, before Update() flushes the
		// per-view outbound queues to the pages.
		API::BridgeApi::Get().PumpMainThread();
		// Apply the snapshot now, so the reconcilers below and the frame submitted
		// this tick reflect the new menu state.
		ApplyPresentationRequests(presentationWork);
		// Land coalesced settings value writes once their write-behind window
		// elapses — a slider drag costs one disk write per
		// ~500ms, not one per step.
		if (_settings) {
			_settings->Store().PumpPersistence(_uptime);
			// Schema hot-reload (developer mode): edited
			// settings/*.json files reload live, values preserved; the
			// registry re-broadcast repaints open Mod Settings.
			if (_config.devMode) {
				_settings->PumpSchemaHotReload(_uptime);
				if (_uptime >= _nextLocalizationScan) {
					_nextLocalizationScan = _uptime + SettingsModule::kHotReloadScanSeconds;
					if (_localization.ReloadIfChanged()) {
						RefreshLocalizedData();
					}
				}
			}
		}
		// Reconcile engine menu-mode + control-disable toward the derived capture
		// state (not visibility): an open HUD must not disable controls.
		ReconcileFocusMenu();
		// Unconditional, so losing capture releases any engaged lock (a gate here
		// would stop reconciling and strand the player's controls).
		ReconcileControlLayer();
		// Sim pause (manifest pausesGame) — unconditional: a direct
		// Main::isGameMenuPaused write, independent of the engine focus menu.
		ReconcileSimPause();
		// OS-cursor release — unconditional, tracks capture (the same policy that
		// activates the hardware cursor): while a menu captures input, hold a
		// reference on MenuCursor::freeCursorRefCount so the per-frame clip
		// releases the pointer (no engine arrow — the focus menu carries no
		// ShowCursor bit). Edge-triggered inside Apply.
		FreeCursor::Apply(_presentation.DesiredCapture());
		DrainEngineInput(a_deltaSeconds);
		if (!_renderer) {
			return;
		}
		// Fire any due crash-recovery reloads before Update pumps the renderer.
		DriveRecovery();
		DriveViewLifecycle();
		DriveDevTools();
		PumpDevViewReload();
		// Flush the coalesced mouse move (QueueMouseMove): one injected move
		// per frame carrying the latest position, however many raw packets the
		// window thread recorded since the last tick.
		if (const auto packed = _pendingMouseMove.exchange(kNoPendingMouseMove);
			packed != kNoPendingMouseMove) {
			_renderer->InjectMouseMove(
				static_cast<int>(packed >> 32),
				static_cast<int>(packed & 0xFFFF'FFFFull));
			++_mouseMoveSends;
		}
		if (_config.devMode && _uptime >= _nextMouseStatsLog) {
			_nextMouseStatsLog = _uptime + 5.0;
			const auto packets = _mouseMovePackets.exchange(0, std::memory_order_relaxed);
			if (packets != 0 || _mouseMoveSends != 0) {
				REX::DEBUG("Runtime: coalesced {} mouse-move packets into {} sends over ~5s",
					packets, _mouseMoveSends);
				_mouseMoveSends = 0;
			}
		}
		{
			// Out-of-process renderers mirror the accelerator state so their browser host
			// process can decide `handled` synchronously; pushed every tick,
			// web renderer implementations diff and forward only changes (default no-op).
			_renderer->SetAcceleratorKeys(_toggleKey.load(std::memory_order_acquire),
				IsInputCaptured(), _captureArmed.load(), _captureUpScan.load());
			_renderer->Update(a_deltaSeconds);
			DrivePendingOpen();
			SubmitFrameIfVisible();
		}
		// After Update(), so health edges raised by either renderer this tick are
		// in the registry before the snapshot goes out.
		_runtimeHealth.Pump();
	}

	void Runtime::EnqueuePresentationRequest(PresentationRequest a_req)
	{
		// Callable from any thread (WndProc toggle/Esc, MenuEventSink transition).
		// Leaf lock: it only guards the queue; the request is acted on in Tick.
		std::lock_guard lock(_reqMutex);
		_presentationRequests.push_back(a_req);
	}

	void Runtime::EnqueueOpenView(std::string a_viewId)
	{
		// Callable from any thread (PauseMenuEntry click). Same leaf-lock
		// discipline as EnqueuePresentationRequest.
		std::lock_guard lock(_reqMutex);
		_openViewReqs.push_back(std::move(a_viewId));
	}

	bool Runtime::InstantiateView(const ViewManifest& a_manifest, std::string_view a_reason)
	{
		const auto& id = a_manifest.id;
		if (_presentation.IsInstantiated(id)) {
			return true;
		}
		if (!_renderer) {
			return false;
		}

		// Install console capture before navigation so even the earliest page console
		// output is captured. The handler survives recovery reloads until the view
		// is explicitly destroyed.
		if (_config.devMode) {
			_renderer->SetConsoleHandler(id, [id](int a_level, std::string a_message) {
				if (a_level == 2) {
					REX::ERROR("Runtime: view '{}' console: {}", id, a_message);
				} else if (a_level == 1) {
					REX::WARN("Runtime: view '{}' console: {}", id, a_message);
				} else {
					// console.log/info/debug: page chatter, not a diagnosis signal —
					// keep it out of the INFO band even in devMode.
					REX::DEBUG("Runtime: view '{}' console: {}", id, a_message);
				}
			});
		}

		_recovery.erase(id);
		_viewLoadState[id] = ViewLoadState::Loading;
		_contentReadyViews.erase(id);
		_renderer->CreateOrNavigateView(a_manifest);
		// A fresh view starts at manifest dimensions; restore the current
		// output-matched size. Before first present these are the initialized
		// logical dimensions and the normal output-resize path supersedes them.
		if (const auto w = _viewWidth.load(), h = _viewHeight.load(); w && h) {
			_renderer->Resize(w, h);
		}
		_presentation.AddInstantiated({ id, a_manifest.kind, a_manifest.capturesInput,
			a_manifest.pausesGame, a_manifest.order });
		_viewLifecycle.NoteInstantiated(id, _pinnedViews.contains(id), _uptime);
		API::BridgeApi::Get().SetViewInstantiated(id, true);

		REX::INFO("Runtime: view '{}' instantiated {} ({}, capturesInput={}, pausesGame={})",
			id, a_reason, a_manifest.kind == ViewKind::Hud ? "hud" : "menu",
			a_manifest.capturesInput, a_manifest.pausesGame);
		if (a_manifest.permissions.nativeBridge && _bridge) {
			// This may be the first bridge-enabled view. Publish the bridge before
			// this tick's PumpMainThread so queued sends reach the newly created
			// renderer view.
			API::BridgeApi::Get().SetBridgeAvailability(_bridge.get());
			// Arm a closed event gate. The greeting is the PAGE's move now, so
			// nothing is pushed here: events raised before the document says hello
			// queue behind the gate, and every current state value is replayed when
			// it does. That is the whole boot path, identically for a first open, an
			// F5, a dev hot-reload and a crash-recovery reload.
			_bridge->OnViewCreated(id, IsPre2Target(a_manifest.targetVersion));
		}
		return true;
	}

	Runtime::PendingPresentationWork Runtime::TakePresentationRequests()
	{
		// Snapshot under the lock, then act unlocked (in ApplyPresentationRequests): the
		// actions call into the renderer/compositor and must never run while
		// holding _reqMutex.
		PendingPresentationWork work;
		{
			std::lock_guard lock(_reqMutex);
			work.local.swap(_presentationRequests);
			work.openViews.swap(_openViewReqs);
		}
		// Sibling-plugin opens/closes by id; same policy path as the toggle key.
		work.plugin = API::BridgeApi::Get().TakeViewPresentationRequests();
		return work;
	}

	void Runtime::PreparePresentationRequests(const PendingPresentationWork& a_work)
	{
		const auto prepare = [this](std::string_view a_id, std::string_view a_reason) {
			if (_presentation.IsInstantiated(a_id)) {
				return;
			}
			if (const auto* manifest = _views.Find(a_id)) {
				InstantiateView(*manifest, a_reason);
			}
		};

		for (const auto& id : a_work.openViews) {
			prepare(id, "on demand");
		}
		for (const auto& request : a_work.plugin) {
			if (request.open) {
				prepare(request.view, "on demand");
			}
		}
		if (!_pendingViewOpen && !_presentation.ActiveMenu() &&
			std::ranges::find(a_work.local, PresentationRequest::ToggleDefault) != a_work.local.end()) {
			prepare(_config.view, "for the default-menu toggle");
		}
	}

	void Runtime::ApplyPresentationRequests(const PendingPresentationWork& a_work)
	{
		const auto& reqs = a_work.local;
		const auto& pluginReqs = a_work.plugin;
		if (reqs.empty() && pluginReqs.empty() && a_work.openViews.empty()) {
			return;
		}
		for (const auto req : reqs) {
			switch (req) {
			case PresentationRequest::ToggleDefault:
				if (_pendingViewOpen) {
					CancelPendingOpen();
				} else if (_presentation.ActiveMenu()) {
					_presentation.CloseActiveMenu();
				} else {
					BeginViewOpen(_config.view);
				}
				break;
			case PresentationRequest::Back: {
				// Esc / pad-B. A back-owning active menu (osfui.handleBack) gets
				// the action delegated as a synthetic Escape tap and decides for
				// itself — navigate elsewhere, peel an inner panel, or send
				// `close`. Everyone else closes the active menu (single-menu policy:
				// that hides the overlay). The toggle key never delegates, so a
				// broken page cannot strand the user.
				const auto active = _presentation.ActiveMenu();
				if (_pendingViewOpen && (!active || *active == kHandoffViewId)) {
					CancelPendingOpen();
				} else if (active && _backOwnerViews.contains(*active) && _renderer) {
					constexpr std::uint32_t kVkEscape = 0x1B;
					_renderer->InjectKeyEvent(kVkEscape, true);
					_renderer->InjectKeyEvent(kVkEscape, false);
				} else {
					_presentation.CloseActiveMenu();
				}
				break;
			}
			case PresentationRequest::CloseAll:
				CancelPendingOpen();
				_presentation.CloseAll();
				break;
			}
		}
		for (const auto& id : a_work.openViews) {
			if (!_presentation.IsInstantiated(id)) {
				REX::WARN("Runtime: EnqueueOpenView('{}') ignored — no discovered view could be instantiated", id);
			} else {
				BeginViewOpen(id);
			}
		}
		for (const auto& r : pluginReqs) {
			if (r.open) {
				if (!_presentation.IsInstantiated(r.view)) {
					REX::WARN("Runtime: plugin RequestMenu('{}', open) could not instantiate the discovered view", r.view);
				} else {
					BeginViewOpen(r.view);
				}
			} else {
				if (_pendingViewOpen &&
					(_pendingViewOpen->target == r.view || r.view == kHandoffViewId)) {
					CancelPendingOpen();
				}
				_presentation.Close(r.view);
			}
		}
		ApplyViewPresentationPolicy();
	}

	bool Runtime::OverlayCanDraw() const
	{
		return _overlayDrawAvailable.load(std::memory_order_acquire) && UiPassSeam::DrawEnabled();
	}

	bool Runtime::BeginViewOpen(std::string_view a_id)
	{
		// Both halves: Install() only proves the vtable hooks were taken, while
		// the command-list hooks are self-tested lazily on a render worker and
		// can disable the seam afterwards. Gating on the install alone admits an
		// invisible overlay that still holds focus and input.
		if (!OverlayCanDraw()) {
			REX::WARN("Runtime: cannot open '{}' — the Scaleform UI draw path is unavailable",
				a_id);
			return false;
		}
		if (_rendererFailed) {
			if (_browserHostRecovery.RequestManualRetry(_uptime)) {
				REX::INFO("Runtime: open of '{}' requested a fresh browser-host recovery cycle; "
					"the overlay remains closed until the replacement reaches its reveal gate", a_id);
			} else if (_browserHostRecovery.PhaseValue() ==
				BrowserHostRecovery::Phase::Waiting ||
				_browserHostRecovery.PhaseValue() ==
				BrowserHostRecovery::Phase::AwaitingResponse) {
				REX::WARN("Runtime: cannot open '{}' yet - the browser host is recovering", a_id);
			} else {
				REX::WARN("Runtime: cannot open '{}' - the web renderer needs a game restart or "
					"the repair described in the log", a_id);
			}
			return false;
		}
		if (!_presentation.IsInstantiated(a_id)) {
			return false;
		}
		const auto* manifest = _views.Find(a_id);
		if (manifest && manifest->kind == ViewKind::Menu && manifest->capturesInput &&
			!_captureIntegrationAvailable) {
			REX::WARN("Runtime: cannot open '{}' — required input integration is unavailable", a_id);
			return false;
		}
		if (!manifest || manifest->kind == ViewKind::Hud ||
			a_id == kHandoffViewId || !_presentation.IsInstantiated(kHandoffViewId)) {
			CancelPendingOpen();
			return _presentation.Open(a_id);
		}

		const auto loadState = GetViewLoadState(a_id);
		if (IsViewRevealReady(a_id, *manifest, loadState)) {
			CancelPendingOpen();
			return _presentation.Open(a_id);
		}
		if (_pendingViewOpen && _pendingViewOpen->target == a_id) {
			return false;
		}

		CancelPendingOpen();
		PendingViewOpen pending;
		pending.target = std::string(a_id);
		pending.startedAt = _uptime;
		if (loadState == ViewLoadState::Finished) {
			pending.loadedAt = _uptime;
		}
		_pendingViewOpen = std::move(pending);
		REX::DEBUG("Runtime: holding first open of '{}' until its reveal gate is reached", a_id);
		return true;
	}

	bool Runtime::CancelPendingOpen()
	{
		if (!_pendingViewOpen) {
			return false;
		}
		const auto target = _pendingViewOpen->target;
		const bool changed = _presentation.Close(kHandoffViewId);
		_pendingViewOpen.reset();
		REX::DEBUG("Runtime: cancelled pending open of '{}'", target);
		return changed;
	}

	void Runtime::ShowHandoff(std::string_view a_phase, bool a_retry)
	{
		if (!_pendingViewOpen || !_bridge) {
			return;
		}
		auto& pending = *_pendingViewOpen;
		const auto* target = _views.Find(pending.target);
		if (!target || !_presentation.IsInstantiated(kHandoffViewId)) {
			return;
		}
		const bool stateChanged = !pending.handoffVisible || pending.phase != a_phase ||
			pending.error != a_retry;
		if (!stateChanged) {
			return;
		}

		// The pinned handoff view borrows the target menu's policy, so loading feels
		// like entering that same target view instead of opening global UI chrome.
		_presentation.AddInstantiated({ std::string(kHandoffViewId), ViewKind::Menu,
			target->capturesInput, target->pausesGame, target->order });
		const auto title = _localization.Resolve(target->mod,
			"views." + std::string(Ids::ViewNameOf(target->id)) + ".title", target->title);
		// STATE, not an event: this is latest-wins data the handoff view
		// renders from. As a push it left the view showing its cold pre-state
		// look forever after an F5, because nothing re-sent it.
		_handoffState = nlohmann::json{
			{ "target", target->id },
			{ "mod", target->mod },
			{ "title", title },
			{ "accent", target->accent },
			{ "phase", a_phase },
			{ "retry", a_retry },
		};
		_bridge->PublishState(kHandoffViewId, "osfui", "handoff", _handoffState);
		_presentation.Open(kHandoffViewId);
		pending.handoffVisible = true;
		pending.phase = std::string(a_phase);
		pending.error = a_retry;
		ApplyViewPresentationPolicy();
	}

	void Runtime::FinishPendingOpen()
	{
		if (!_pendingViewOpen) {
			return;
		}
		const auto target = _pendingViewOpen->target;
		_presentation.Close(kHandoffViewId);
		_presentation.Open(target);
		_pendingViewOpen.reset();
		REX::DEBUG("Runtime: first-load handoff completed for '{}'", target);
		ApplyViewPresentationPolicy();
	}

	void Runtime::DrivePendingOpen()
	{
		if (!_pendingViewOpen) {
			return;
		}
		auto& pending = *_pendingViewOpen;
		const auto* manifest = _views.Find(pending.target);
		if (!manifest) {
			ShowHandoff("error", true);
			return;
		}
		// An uninstantiated target is exactly the state the retry exists to
		// recover from (OnViewLoad's exhaustion path destroys the view and
		// removes its instance), so only park on the error screen when no retry is
		// pending.
		if (!_presentation.IsInstantiated(pending.target) && !pending.retryRequested) {
			ShowHandoff("error", true);
			return;
		}
		if (pending.retryRequested) {
			pending.retryRequested = false;
			if (!_renderer) {
				return;
			}
			if (!_presentation.IsInstantiated(pending.target)) {
				if (!InstantiateView(*manifest, "for first-load handoff retry")) {
					ShowHandoff("error", true);
					return;
				}
			} else {
				_recovery.erase(pending.target);
				// ReloadViewInPlace re-arms the bridge `ready` handshake itself now, for every
				// reload path rather than only this one.
				ReloadViewInPlace(pending.target, *manifest);
			}
			pending.startedAt = _uptime;
			pending.loadedAt = -1.0;
			pending.phase.clear();
			pending.error = false;
			ShowHandoff("linking", false);
			BroadcastViewsData();
			return;
		}

		const auto state = GetViewLoadState(pending.target);
		if (state == ViewLoadState::Finished && pending.loadedAt < 0.0) {
			pending.loadedAt = _uptime;
		}
		if (IsViewRevealReady(pending.target, *manifest, state)) {
			FinishPendingOpen();
			return;
		}
		if (manifest->readySignal && pending.loadedAt >= 0.0 &&
			_uptime - pending.loadedAt >= kReadySignalTimeoutSeconds) {
			ShowHandoff("error", true);
			return;
		}
		if (_uptime - pending.startedAt < kHandoffDelaySeconds) {
			return;
		}
		ShowHandoff(state == ViewLoadState::Failed ? "retrying" : "linking", false);
	}

	void Runtime::RetryPendingOpen()
	{
		if (_pendingViewOpen && _pendingViewOpen->error) {
			_pendingViewOpen->retryRequested = true;
		}
	}

	void Runtime::DrainSchemaOps()
	{
		if (!_settings) {
			return;  // no store yet — ops keep waiting in BridgeApi's queue
		}
		auto ops = API::BridgeApi::Get().TakeSchemaOps();
		if (ops.empty()) {
			return;
		}
		auto& store = _settings->Store();
		for (auto& op : ops) {
			if (!op.schema.is_null()) {
				// Shape was validated synchronously at the ABI boundary; what's
				// left here is precedence (native wins, logged inside).
				store.RegisterSchema(std::move(op.schema), SettingsStore::Source::kNative);
			} else if (store.GetSource(op.modId) == SettingsStore::Source::kNative) {
				store.RemoveMod(op.modId);
			} else {
				REX::WARN("Runtime: UnregisterSettingsSchema('{}') ignored — not a native-registered schema", op.modId);
			}
		}
	}

	void Runtime::DrainViewRegistrations()
	{
		auto ids = API::BridgeApi::Get().TakeViewRegistrations();
		if (ids.empty()) {
			return;
		}
		if (!_renderer) {
			// Overlay disabled or never came up: drop loudly rather than
			// queueing forever.
			for (const auto& id : ids) {
				REX::WARN("Runtime: plugin RegisterView('{}') ignored — overlay not running", id);
			}
			return;
		}
		bool catalogChanged = false;
		for (const auto& id : ids) {
			// Idempotent: re-registering an instantiated view (or a repeat
			// call) would blow away its page state.
			if (_presentation.IsInstantiated(id)) {
				REX::DEBUG("Runtime: plugin RegisterView('{}') — already instantiated, left untouched", id);
				continue;
			}
			const auto* m = _views.Find(id);
			if (!m) {
				REX::WARN("Runtime: plugin RegisterView('{}') ignored — no views/{}/manifest.json was discovered at boot (ids are qualified '<author>.<modname>/<view>'; is the view folder installed?)", id, id);
				continue;
			}
			if (m->openOnStart) {
				if (!InstantiateView(*m, "via plugin RegisterView openOnStart")) {
					continue;
				}
				_presentation.Open(id);
				catalogChanged = true;
			} else {
				// Discovery already made this id catalogued and RequestMenu-openable.
				// RegisterView now validates intent while deferring page creation.
				REX::DEBUG("Runtime: plugin RegisterView('{}') accepted; creation deferred until first open", id);
			}
		}
		if (catalogChanged) {
			ApplyViewPresentationPolicy();     // openOnStart / z-band changes take effect now
			BroadcastViewsData();  // Mod Settings picks the new view up live
		}
	}

	void Runtime::ApplyViewPresentationPolicy()
	{
		if (!_renderer) {
			return;
		}
		// All menu-opening paths converge here, including RegisterView and
		// a page asking to show itself. Keep HUD state, but never let an active menu
		// claim focus/input when the compositor cannot put it on screen.
		if (!OverlayCanDraw() && _presentation.ActiveMenu()) {
			REX::WARN("Runtime: closing a requested menu because the Scaleform UI draw path is unavailable");
			_presentation.CloseActiveMenu();
		}
		// A capturing menu is safe only when the whole production input path is
		// available. This central guard also covers plugin openOnStart and a view opening
		// itself through setVisible, which do not pass through BeginViewOpen.
		if (_presentation.DesiredCapture() && !_captureIntegrationAvailable) {
			REX::WARN("Runtime: closing a requested menu because required input integration is unavailable");
			CancelPendingOpen();
			_presentation.CloseActiveMenu();
		}
		// Per-view hidden + composite z, derived from the band order: HUDs
		// by `order` beneath the one active menu.
		for (const auto& layer : _presentation.DesiredLayers()) {
			_renderer->SetViewHidden(layer.id, layer.hidden);
			_viewLifecycle.NoteVisibility(layer.id, !layer.hidden, _uptime);
			_viewLifecycle.NoteOpenState(layer.id, _presentation.IsOpen(layer.id), _uptime);
			_renderer->SetViewOrder(layer.id, layer.z);
		}
		// The renderer's input target follows the active menu; HUD-only means no
		// input target changes.
		const auto active = _presentation.ActiveMenu();
		if (active) {
			_renderer->SetInputTargetView(*active);
		}
		// Capture follows the active menu's policy (false for HUD-only => the game
		// keeps input).
		const bool desiredCapture = _presentation.DesiredCapture();
		const bool captureChanged = _captureInput.exchange(desiredCapture) != desiredCapture;
		if (captureChanged) {
			// Hardware cursor state belongs to the game window thread. Wake it now;
			// a menu-session focus transfer can otherwise happen before the next
			// WM_INPUT packet and leave the OS pointer hidden for the whole session.
			OverlayInputHook::RequestStateRefresh();
			if (!desiredCapture) {
				// Every menu-goes-away path funnels through this edge (mouse
				// "Exit", pad-B, transition CloseAll). An armed rebind must die
				// with the menu, or the next gameplay keypress is captured.
				CancelArmedKeyCapture();
			}
		}

		// Visibility side-effects live here rather than behind a change guard,
		// which would drop the compositor push on the no-change startup path.
		const bool visible = _presentation.DesiredVisible();
		const bool wasVisible = _visible.exchange(visible);
		// Input-capturing menus use real browser focus for the full session so Windows
		// schedules Chromium as foreground work. HUD-only views leave the game
		// focused. Mouse and controller have focus-independent paths for the menu.
		ReconcileNativeFocus();
		if (_compositor) {
			if (visible && !wasVisible) {
				// Closed->open edge: defer the reveal. The compositor redraws
				// its last cached texture every present while visible, so
				// showing it now would flash stale pre-open content for the
				// frames it takes the renderer to deliver queued messages and
				// hand over a post-open frame.
				m_viewReveal.Arm();
			} else {
				if (!visible) {
					m_viewReveal.Cancel();  // closed while a reveal was still pending
				}
				if (!m_viewReveal.Pending()) {
					_compositor->SetVisible(visible);
				}
			}
		}

		// Open->closed edge: flush the settings write-behind instead of waiting
		// out the window (the shutdown flush is
		// ~SettingsStore).
		if (!visible && wasVisible && _settings) {
			_settings->Store().FlushPersistence();
		}

		// Recenter the virtual cursor on the closed->open edge, else keep its
		// position; either way (re)place it in the active menu so a freshly
		// focused view shows it at the right spot, not its stale origin.
		if (visible) {
			if (!wasVisible) {
				_cursorX = _viewWidth.load() * 0.5f;
				_cursorY = _viewHeight.load() * 0.5f;
			}
			if (active) {
				QueueMouseMove();  // flushed by Tick's once-per-frame move injection
			}
		}
		// ui.visibility keys off the shown view (the active menu of a visible
		// overlay) changing, not off the overlay's open/close edge: a view switch
		// while the overlay stays up (Mod Settings -> another view) is a real show for the new view
		// and a real hide for the old one. Consumers arm whole sessions off this
		// signal, so an edge-only send left Mod Settings-opened views permanently "closed".
		// The hide can't render a fade-out (the compositor already hid this frame
		// on the overlay-close path), but the view's JS keeps running while hidden.
		// By overlay close ActiveMenu() is already empty, hence the tracked name.
		if (_bridge) {
			const std::string shown = (visible && active) ? *active : std::string();
			if (shown != _lastShownView) {
				// reason lets views scope per-overlay-visit state to real overlay
				// edges while still seeing focus handoffs: "overlay" = the overlay
				// opened/closed this tick, "focus" = only the active menu changed.
				const char* reason = (visible == wasVisible) ? "focus" : "overlay";
				if (!_lastShownView.empty()) {
					_bridge->Emit(_lastShownView, "ui.visibility",
						nlohmann::json{ { "visible", false }, { "reason", reason } });
				}
				if (!shown.empty()) {
					_bridge->Emit(shown, "ui.visibility",
						nlohmann::json{ { "visible", true }, { "reason", reason } });
				}
				_lastShownView = shown;
			}
		}
		if (visible != wasVisible) {
			REX::INFO("Runtime: overlay visibility -> {} (capture={})", visible, _captureInput.load());
		}

		BroadcastViewsData();
	}

	void Runtime::ReconcileNativeFocus()
	{
		// Main thread (Runtime::Tick).
		// Edge-guarded: the false side posts a game-focus restore to the window
		// thread, and the true side races Chromium's async MoveFocus, so repeat
		// sends would only feed the focus watchdog more churn.
		if (!_renderer) {
			return;
		}
		const auto active = _presentation.ActiveMenu();
		// A capturing menu owns native focus for its whole visible session. This
		// reproduces Windows' smooth foreground scheduling without changing GPU
		// priorities. HUD-only views have no active capturing menu and leave the
		// game focused.
		const bool want = _visible.load() && _captureInput.load() && active.has_value();
		if (want == _nativeFocusGranted) {
			return;
		}
		_nativeFocusGranted = want;
		_renderer->SetNativeFocus(want);
	}

	bool Runtime::IsVisible() const
	{
		return _visible.load();
	}

	bool Runtime::SetViewHidden(std::string_view a_id, bool a_hidden)
	{
		// The renderer would silently no-op an unknown id; reject for a clear
		// log. Validate against the instantiated presentation registry — every sibling view
		// operation does — not the boot list, which a drop-in view opened via
		// menu.open is never on.
		if (!_presentation.IsInstantiated(a_id)) {
			REX::WARN("Runtime: setViewHidden ignored — '{}' is not an instantiated view", a_id);
			return false;
		}
		if (_renderer) {
			_renderer->SetViewHidden(a_id, a_hidden);
		}
		// Keep lifecycle policy in step with this out-of-band visibility edge,
		// exactly like ApplyViewPresentationPolicy does for policy-driven layers. Without it
		// a view revealed here still ages as hidden and idle reclaim would
		// destroy it while it is on screen (and the suspend handshake desyncs:
		// the browser host refuses a suspend for a visible page the game thinks hidden).
		_viewLifecycle.NoteVisibility(a_id, !a_hidden, _uptime);
		REX::DEBUG("Runtime: view '{}' hidden -> {}", a_id, a_hidden);
		return true;
	}

	void Runtime::OnViewLoad(std::string_view a_viewId, bool a_failed, std::string_view a_url,
		std::string_view a_description, int a_errorCode)
	{
		const std::string id(a_viewId);
		if (_rendererFailed && _browserHostRecovery.CanAcceptResponse()) {
			const auto attempts = _browserHostRecovery.Attempts();
			_browserHostRecovery.Reset();
			_rendererFailed = false;
			_rendererFailureLatched = false;
			REX::INFO("Runtime: replacement browser host responded on attempt {}; "
					  "the overlay remains closed until the player opens it",
				attempts);
		}
		_viewLoadState[id] = a_failed ? ViewLoadState::Failed : ViewLoadState::Finished;
		// The gamepad-raw and back-owner grants are sticky for a page's lifetime,
		// so a (re)loaded page starts un-granted and re-asserts in its own boot code.
		_gamepadRawViews.erase(id);
		_backOwnerViews.erase(id);
		if (!a_failed) {
			// A healthy load clears the strikes, so a later failure gets the full
			// retry budget again.
			if (_recovery.erase(id) > 0) {
				REX::INFO("Runtime: view '{}' recovered ({})", a_viewId, a_url);
			} else {
				REX::INFO("Runtime: view '{}' finished loading ({})", a_viewId, a_url);
			}
			_runtimeHealth.ReportViewLoad(a_viewId, false, {}, 0, 0);
			BroadcastViewsData();  // loadState loading -> loaded
			return;
		}

		REX::ERROR("Runtime: view '{}' FAILED to load ({}): {} [{}]",
			a_viewId, a_url, a_description, a_errorCode);

		// Crash-recovery: schedule a bounded reload with backoff. attempts counts
		// reloads already fired; an exhausted budget means the content is broken,
		// so tear down and unregister the view — otherwise the toggle
		// key / menu.open can re-open an invisible, input-capturing shell.
		constexpr std::uint32_t kMaxAttempts = 3;
		constexpr double        kBackoffSec[kMaxAttempts] = { 2.0, 5.0, 15.0 };
		auto& rec = _recovery[id];
		if (rec.attempts >= kMaxAttempts) {
			REX::ERROR("Runtime: view '{}' still failing after {} reload attempts; giving up — "
					   "destroying and unregistering the view (fix its files and relaunch)",
				a_viewId, rec.attempts);
			// The retry budget is spent: this is the error a player has to act on.
			_runtimeHealth.ReportViewLoad(a_viewId, true, a_description, a_errorCode, 0);
			TearDownView(id, ViewTeardownReason::LoadExhausted);
			return;
		}
		rec.pending = true;
		rec.retryAt = _uptime + kBackoffSec[rec.attempts];
		REX::WARN("Runtime: view '{}' reload attempt {}/{} scheduled in {:.0f}s",
			a_viewId, rec.attempts + 1, kMaxAttempts, kBackoffSec[rec.attempts]);
		_runtimeHealth.ReportViewLoad(a_viewId, true, a_description, a_errorCode,
			kMaxAttempts - rec.attempts);
		BroadcastViewsData();  // loadState -> failed
	}

	bool Runtime::IsViewRevealReady(std::string_view a_id, const ViewManifest& a_manifest, ViewLoadState a_state) const
	{
		return a_manifest.readySignal ? _contentReadyViews.contains(std::string(a_id)) :
										a_state == ViewLoadState::Finished;
	}

	void Runtime::ReloadViewInPlace(const std::string& a_id, const ViewManifest& a_manifest)
	{
		_viewLoadState[a_id] = ViewLoadState::Loading;
		_contentReadyViews.erase(a_id);
		_viewLifecycle.NoteActivity(a_id, _uptime);
		_renderer->CreateOrNavigateView(a_manifest);
		if (a_manifest.permissions.nativeBridge && _bridge) {
			// Re-arm the gate: the replacement document greets the bridge itself and
			// is replayed then. This is where 1.x had to race a browser-host-initiated
			// greeting against the navigate (and lean on the browser host's domSeen reset to
			// keep it off the outgoing page) — a page-initiated handshake cannot
			// reach the wrong document by construction.
			_bridge->OnViewCreated(a_id, IsPre2Target(a_manifest.targetVersion));
		}
		// A recreated view starts at manifest dimensions; restore the
		// output-matched size so it composites 1:1 again.
		_renderer->Resize(_viewWidth.load(), _viewHeight.load());
	}

	void Runtime::DriveRecovery()
	{
		if (_rendererFailed || _recovery.empty() || !_renderer) {
			return;
		}
		for (auto& [id, rec] : _recovery) {
			if (!rec.pending || _uptime < rec.retryAt) {
				continue;
			}
			rec.pending = false;
			const auto* manifest = _views.Find(id);
			if (!manifest) {
				continue;  // only instantiated views can emit document-load events
			}
			++rec.attempts;
			REX::INFO("Runtime: crash-recovery reloading view '{}' (attempt {})", id, rec.attempts);
			ReloadViewInPlace(id, *manifest);
		}
	}

	void Runtime::DriveViewLifecycle()
	{
		if (_rendererFailed || !_renderer) {
			return;
		}
		const auto actions = _viewLifecycle.CollectDueActions(_uptime);
		const auto unavailable = [this](const std::string& a_id) {
			return !_presentation.IsInstantiated(a_id) ||
			       GetViewLoadState(a_id) == ViewLoadState::Loading ||
			       _recovery.contains(a_id) ||
			       (_pendingViewOpen && _pendingViewOpen->target == a_id);
		};
		for (const auto& id : actions.suspend) {
			if (unavailable(id) ||
				(id == kHandoffViewId && _pendingViewOpen.has_value())) {
				continue;
			}
			_renderer->SuspendView(id);
			_viewLifecycle.NoteSuspendRequested(id);
		}
		for (const auto& id : actions.destroy) {
			if (unavailable(id) || _presentation.IsOpen(id)) {
				continue;
			}
			TearDownView(id, ViewTeardownReason::IdleReclaim);
		}
	}

	void Runtime::TearDownView(const std::string& a_id, ViewTeardownReason a_reason)
	{
		_recovery.erase(a_id);
		_contentReadyViews.erase(a_id);
		if (a_reason == ViewTeardownReason::IdleReclaim) {
			_viewLoadState.erase(a_id);
		}
		if (_renderer) {
			_renderer->DestroyView(a_id);
		}
		if (_presentation.RemoveInstantiated(a_id)) {
			ApplyViewPresentationPolicy();  // crash teardown may need to release input/pause now
		}
		API::BridgeApi::Get().SetViewInstantiated(a_id, false);
		bool bridgeViewRemains = false;
		for (const auto& manifest : _views.All()) {
			if (manifest.permissions.nativeBridge && _presentation.IsInstantiated(manifest.id)) {
				bridgeViewRemains = true;
				break;
			}
		}
		if (!bridgeViewRemains) {
			API::BridgeApi::Get().SetBridgeAvailability(nullptr);
		}
		if (_bridge) {
			// Drops the view's event gate and reaps every request it still owns.
			_bridge->OnViewDestroyed(a_id);
		}
		_gamepadRawViews.erase(a_id);
		_backOwnerViews.erase(a_id);
		for (const auto& mod : _modules) {
			mod->OnViewDestroyed(a_id);
		}
		_viewLifecycle.NoteDestroyed(a_id);
		if (a_reason == ViewTeardownReason::IdleReclaim) {
			REX::INFO("Runtime: reclaimed idle view '{}' after {:.0f} minutes hidden; it will be reinstantiated on next open",
				a_id, ViewLifecycle::kDestroyAfterHiddenSeconds / 60.0);
		}
		BroadcastViewsData();
	}

	void Runtime::DriveDevTools()
	{
		if (!_devToolsRequested.exchange(false) || !_renderer || !_config.devMode) {
			return;
		}
		const auto active = _presentation.ActiveMenu();
		if (!active) {
			REX::DEBUG("Runtime: F12 DevTools — no open menu to inspect");
			return;
		}
		REX::INFO("Runtime: opening DevTools for view '{}'", *active);
		_renderer->OpenDevTools(*active);
	}

	void Runtime::PumpDevViewReload()
	{
		if (!_devViewReload) return;

		std::vector<DevViewReloadWorker::Target> targets;
		for (const auto& manifest : _views.All()) {
			if (_presentation.IsInstantiated(manifest.id)) {
				targets.push_back({ manifest.id });
			}
		}
		_devViewReload->SetTargets(std::move(targets));

		bool anyReloaded = false;
		for (const auto& completed : _devViewReload->DrainCompleted()) {
			const auto* manifest = _views.Find(completed.id);
			if (!manifest || !_presentation.IsInstantiated(completed.id)) continue;
			ReloadViewInPlace(completed.id, *manifest);
			anyReloaded = true;
			REX::INFO("Runtime: dev reloaded loose view '{}'", completed.id);
		}
		if (anyReloaded) BroadcastViewsData();
	}

	bool Runtime::HudAutoStartEligible(const ViewManifest& a_manifest) const
	{
		return a_manifest.kind == ViewKind::Hud &&
		       !_pinnedViews.contains(a_manifest.id) &&
		       a_manifest.catalogVisible && (!a_manifest.debugOnly || _config.devMode);
	}

	nlohmann::json Runtime::BuildViewsData() const
	{
		nlohmann::json views = nlohmann::json::array();
		const auto     active = _presentation.ActiveMenu();
		for (const auto& m : _views.All()) {
			// Every discovered manifest is a launchable view, so list them all.
			// An instantiated view carries its current main-frame load state; a discovered-but-
			// uninstantiated one is reported "unloaded" so Mod Settings can show
			// it as a click-to-instantiate card (the click's menu.open instantiates it
			// on demand through EnqueueOpenView). A view whose recovery was exhausted stays
			// "failed" — its _viewLoadState entry survives removal, so it is
			// caught below before the instantiated/unloaded split. Catalog-hidden
			// (`hub:false`) and debugOnly views are still withheld here.
			const bool instantiated = _presentation.IsInstantiated(m.id);
			const auto state = GetViewLoadState(m.id);
			const char* loadState =
				state == ViewLoadState::Failed   ? "failed" :
				state == ViewLoadState::Finished ? "loaded" :
				instantiated                     ? "loading" :
				                                   "unloaded";
			// Startup-policy fields introduced in web bridge protocol 1.6. `autoStart` is the effective
			// choice for the NEXT launch; pinned core views always run and are
			// never player-configurable.
			const bool pinned = _pinnedViews.contains(m.id);
			const bool autoStartMutable = HudAutoStartEligible(m);
			const bool autoStart = pinned ||
				(autoStartMutable && _viewPolicy.HudAutoStart(m.id, m.openOnStart));
			views.push_back(nlohmann::json{
				{ "id", m.id },
				{ "title", _localization.Resolve(m.mod,
					"views." + std::string(Ids::ViewNameOf(m.id)) + ".title", m.title) },
				{ "description", _localization.Resolve(m.mod,
					"views." + std::string(Ids::ViewNameOf(m.id)) + ".description", m.description) },
				{ "mod", m.mod },
				{ "kind", m.kind == ViewKind::Hud ? "hud" : "menu" },
				{ "interactive", m.menuInputEligible },
				{ "hub", m.catalogVisible && (!m.debugOnly || _config.devMode) },
				{ "targetVersion", m.targetVersion },
				{ "open", _presentation.IsOpen(m.id) },
				{ "focused", active.has_value() && *active == m.id },
				{ "loadState", loadState },
				{ "autoStart", autoStart },
				{ "autoStartMutable", autoStartMutable },
				{ "pinned", pinned },
			});
		}
		return nlohmann::json{ { "views", std::move(views) } };
	}

	void Runtime::BroadcastViewsData()
	{
		if (!_bridge) {
			return;
		}
		// Content dedupe: callers invoke this unconditionally after any
		// potentially-catalog-changing event. The hello replay deliberately does
		// NOT come through here — it publishes _lastViewsData directly — because
		// a dedupe against the last CHANGE would send the second view to connect
		// nothing at all.
		auto dumped = Json::Dump(BuildViewsData());
		if (dumped == _lastViewsData) {
			return;
		}
		_lastViewsData = std::move(dumped);
		PublishPlatformState("views");
	}

	std::unordered_set<std::string> Runtime::InstantiatedViewsOfMod(std::string_view a_mod) const
	{
		std::unordered_set<std::string> targets;
		for (const auto& manifest : _views.All()) {
			if (!_presentation.IsInstantiated(manifest.id)) {
				continue;
			}
			// Case-INSENSITIVE. A Papyrus mod id arrives through BSFixedString
			// interning, which hands back the first casing the process saw,
			// while a view id is lowercase by grammar. 1.x matched
			// case-sensitively here and case-insensitively on replay, so a mod
			// whose folder case differed from its script's spelling got its
			// state on reload and never on an immediate push.
			if (Ids::EqualsCaseInsensitiveAscii(Ids::ModOf(manifest.id), a_mod)) {
				targets.insert(manifest.id);
			}
		}
		return targets;
	}

	void Runtime::PublishModState(std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value)
	{
		if (!_bridge) {
			return;
		}
		const auto targets = InstantiatedViewsOfMod(a_mod);
		if (targets.empty()) {
			// Not an error, and not a lost write: the value is retained, so the
			// mod's first view is replayed it the moment it greets the bridge.
			REX::DEBUG("Runtime: state '{}/{}' has no instantiated view yet — retained for the next greeting",
				a_mod, a_key);
			return;
		}
		_bridge->PublishState(targets, a_mod, a_key, a_value);
	}

	void Runtime::PublishPlatformState(std::string_view a_key, std::string_view a_viewId)
	{
		if (!_bridge) {
			return;
		}
		const auto deliver = [&](const std::string& a_view) {
			if (a_key == "views") {
				if (_lastViewsData.empty()) {
					_lastViewsData = Json::Dump(BuildViewsData());
				}
				_bridge->PublishJsonState(a_view, "osfui", "views", _lastViewsData);
			} else if (a_key == "settings") {
				if (_settings) {
					_bridge->PublishState(a_view, "osfui", "settings", _settings->Store().DataView());
				}
			} else if (a_key == "diagnostics") {
				if (_healthRegistry) {
					_bridge->PublishState(a_view, "osfui", "diagnostics", _healthRegistry->Snapshot());
				}
			} else if (a_key == "keybindings") {
				_bridge->PublishState(a_view, "osfui", "keybindings", _controlMap.KeybindingsState());
			} else if (a_key == "input-context") {
				_bridge->PublishState(a_view, "osfui", "input-context", _controlMap.EngineInputContextState());
			} else if (a_key == "i18n") {
				// Computed per view: a view's catalog is its OWNING mod's, which
				// is why this one key carries a different value to each document.
				const std::string mod{ Ids::ModOf(a_view) };
				_bridge->PublishState(a_view, "osfui", "i18n", nlohmann::json{
					{ "mod", mod },
					{ "locale", _localization.Locale() },
					{ "strings", _localization.CatalogFor(mod) },
				});
			}
		};
		if (!a_viewId.empty()) {
			deliver(std::string(a_viewId));
			return;
		}
		// PublishState drops anything addressed to a view that has not greeted
		// the bridge, so this needs no subscriber set to prune: an ungreeted
		// document is replayed everything when it does greet.
		for (const auto& manifest : _views.All()) {
			if (manifest.permissions.nativeBridge && _presentation.IsInstantiated(manifest.id)) {
				deliver(manifest.id);
			}
		}
	}

	void Runtime::OnViewGreeted(std::string_view a_viewId)
	{
		if (!_bridge) {
			return;
		}
		// `ready` is already out and this view's event gate is open, so
		// everything published here precedes the first event the document sees.
		// Nothing below consults a change-dedupe: those exist so a repeated
		// broadcast is cheap, and applying one here would send the second view
		// to connect nothing at all.
		for (const auto* key : { "settings", "views", "diagnostics", "keybindings", "input-context", "i18n" }) {
			PublishPlatformState(key, a_viewId);
		}
		if (a_viewId == kHandoffViewId && !_handoffState.is_null()) {
			_bridge->PublishState(a_viewId, "osfui", "handoff", _handoffState);
		}
		// The document's own mod's retained state, from whichever mod backend
		// published it — Papyrus SetView* or the native ABI's SetViewState.
		const std::string mod{ Ids::ModOf(a_viewId) };
		if (const auto* entries = _retainedState.Find(mod)) {
			for (const auto& entry : *entries) {
				_bridge->PublishState(a_viewId, mod, entry.key, entry.value);
			}
		}
		// A greeting means a FRESH document, which cannot still hold the input
		// grants the previous one asserted. Dropping them here (rather than only
		// in OnViewLoad) also covers an F5 the runtime never hears about.
		_gamepadRawViews.erase(std::string(a_viewId));
		_backOwnerViews.erase(std::string(a_viewId));
	}

	void Runtime::OnProtocolFault(std::string_view a_viewId, std::string_view a_code,
		std::string_view a_message, const nlohmann::json& a_detail, bool a_viewFault)
	{
		// Developer mode: hand it straight back to the offending document so it lands
		// in that view's OWN console — and therefore in F12 DevTools with full
		// object inspection, and in the SFSE log through the browser host's console
		// forwarder. One mechanism, both developer outputs, no second channel.
		if (_config.devMode && _bridge) {
			_bridge->Emit(a_viewId, "osfui.debug.error", nlohmann::json{
				{ "code", std::string(a_code) },
				{ "message", std::string(a_message) },
				{ "detail", a_detail },
			});
		}
		// A release build has no debug channel, so REPETITION is the signal: a
		// view that keeps getting the protocol wrong raises a health issue. A
		// one-off (a stale view naming one dead endpoint at boot) stays out of
		// the player's face.
		constexpr std::uint32_t kProtocolFaultThreshold = 10;
		// Only faults the VIEW caused. An endpoint handler that missed its deadline is
		// reported to the waiting page above, but naming the page in a
		// `view.protocol-misuse` issue would blame the wrong side.
		if (!a_viewFault || a_viewId.empty() || !_healthRegistry) {
			return;
		}
		const auto count = ++_viewProtocolFaultCounts[std::string(a_viewId)];
		if (count != kProtocolFaultThreshold) {
			return;
		}
		_healthRegistry->Upsert({
			.id = std::format("view.protocol-misuse:{}", a_viewId),
			.code = "view.protocol-misuse",
			.severity = HealthRegistry::Severity::Warning,
			// Dotless: Mod Settings reads a dot in `source` as "a mod
			// reported this", and this is the platform reporting about a view.
			.source = "views",
			.subject = std::string(a_viewId),
			.context = nlohmann::json{ { "code", std::string(a_code) }, { "count", count } },
		}, _uptime);
		_healthRegistry->Broadcast();
	}

	Runtime::ViewLoadState Runtime::GetViewLoadState(std::string_view a_id) const
	{
		const auto it = _viewLoadState.find(std::string(a_id));
		return it == _viewLoadState.end() ? ViewLoadState::Loading : it->second;
	}

	void Runtime::DriveBrowserHostRecovery()
	{
		if (_browserHostRecovery.ExpireResponseWait(_uptime)) {
			REX::ERROR("Runtime: replacement browser host produced no load response in {:.0f}s",
				BrowserHostRecovery::kResponseTimeoutSeconds);
			if (_browserHostRecovery.PhaseValue() ==
				BrowserHostRecovery::Phase::Exhausted) {
				REX::ERROR("Runtime: automatic browser-host recovery exhausted; "
						   "the next explicit menu open will start a fresh retry cycle");
			}
		}

		if (!_browserHostRecovery.BeginDueAttempt(_uptime)) {
			return;
		}

		const auto attempt = _browserHostRecovery.Attempts();
		REX::INFO("Runtime: restarting browser host (attempt {}/{})",
			attempt, BrowserHostRecovery::kMaxAttempts);
		if (!_renderer || !_renderer->RestartAfterFailure()) {
			REX::ERROR("Runtime: renderer could not reset its failed browser-host connection");
			_browserHostRecovery.OnAttemptSetupFailed(_uptime);
			if (_browserHostRecovery.PhaseValue() ==
				BrowserHostRecovery::Phase::Exhausted) {
				REX::ERROR("Runtime: automatic browser-host recovery exhausted; "
						   "the next explicit menu open will start a fresh retry cycle");
			}
			return;
		}

		_rendererFailureLatched = false;
		RehydrateRendererAfterRestart();
	}

	void Runtime::RehydrateRendererAfterRestart()
	{
		if (!_renderer || !_bridge) {
			return;
		}

		_recovery.clear();
		_contentReadyViews.clear();
		_gamepadRawViews.clear();
		_backOwnerViews.clear();
		_pendingMouseMove.store(kNoPendingMouseMove);
		m_viewReveal.Reset();
		_nativeFocusGranted = false;
		_viewLifecycle.OnBrowserHostRestart(_uptime);

		std::size_t reloaded = 0;
		for (const auto& manifest : _views.All()) {
			if (!_presentation.IsInstantiated(manifest.id)) {
				continue;
			}
			_viewLoadState[manifest.id] = ViewLoadState::Loading;
			_renderer->CreateOrNavigateView(manifest);
			if (manifest.permissions.nativeBridge) {
				// RestartAfterFailure discarded messages addressed to the dead
				// documents. Each replacement greets the bridge on load and is
				// replayed then; all this has to do is re-arm its gate.
				_bridge->OnViewCreated(manifest.id, IsPre2Target(manifest.targetVersion));
			}
			++reloaded;
		}

		for (const auto& id : _pinnedViews) {
			if (_presentation.IsInstantiated(id)) {
				_renderer->PrewarmView(id);
			}
		}
		_renderer->Resize(_viewWidth.load(), _viewHeight.load());
		_renderer->SetAcceleratorKeys(_toggleKey.load(std::memory_order_acquire),
			false, _captureArmed.load(), _captureUpScan.load());
		ApplyViewPresentationPolicy();
		BroadcastViewsData();
		REX::INFO("Runtime: replayed {} instantiated view(s) to the replacement browser host; "
				  "overlay left closed", reloaded);
	}

	void Runtime::OnRendererFailure(const IWebRenderer::FailureEvent& a_event)
	{
		if (_rendererFailureLatched) {
			return;
		}
		_rendererFailureLatched = true;
		_rendererFailed = true;
		const bool retryableBrowserHostLoss =
			a_event.stage == "host-connection" && _renderer && _renderer->Name() == "webview2";
		if (retryableBrowserHostLoss) {
			_browserHostRecovery.OnRetryableFailure(_uptime);
			REX::ERROR("Runtime: browser-host connection failed for view '{}' (0x{:08X}): {} - "
					   "closing the overlay; bounded browser-host recovery is scheduled",
				a_event.viewId, a_event.errorCode, a_event.description);
			if (_browserHostRecovery.PhaseValue() ==
				BrowserHostRecovery::Phase::Exhausted) {
				REX::ERROR("Runtime: automatic browser-host recovery exhausted; "
						   "the next explicit menu open will start a fresh retry cycle");
			}
		} else {
			_browserHostRecovery.Disable();
			REX::ERROR("Runtime: renderer failed at '{}' for view '{}' (0x{:08X}): {} - "
					   "closing the overlay and disabling it for this session",
				a_event.stage, a_event.viewId, a_event.errorCode, a_event.description);
		}
		_recovery.clear();

		CancelPendingOpen();
		_presentation.CloseAll();
		ApplyViewPresentationPolicy();

		// The fatal callback arrives from renderer Update(), after Tick's normal
		// policy reconciliation. Release every engine-side effect now instead of
		// leaving actors, controls, pause, or the cursor stranded for another frame.
		ReconcileFocusMenu();
		ReconcileControlLayer();
		ReconcileSimPause();
		FreeCursor::Apply(false);
	}

	bool Runtime::IsInputCaptured() const
	{
		return _initialized && _captureInput.load() && _visible.load();
	}

	bool Runtime::OnGameWindowKey(std::uint32_t a_vkCode, ScanCode a_scanCode, bool a_down)
	{
		// Key-rebind capture (armed by settings.captureKey). Grab the next key
		// press and consume it, so pressing the current toggle key (or Esc)
		// rebinds instead of closing the overlay. Only stash the scan here; the
		// apply happens on the main thread in DrainKeyCapture. The matching key-up
		// is swallowed too so it can't leak/route.
		if (_captureArmed.load()) {
			// PrintScreen never delivers a key-DOWN (Windows quirk): while a
			// capture is armed, its release counts as the press.
			constexpr ScanCode kScanPrintScreen = 0xB7;
			if (a_down || a_scanCode == kScanPrintScreen) {
				// A message with no recoverable scan code still consumed the
				// press; latch the unnameable sentinel so the capture answers
				// with a cancel instead of staying armed forever.
				_capturedScan.store(
					a_scanCode != kInvalidScanCode ? a_scanCode : kUnnameableScan);
				_captureArmed.store(false);
				_captureUpScan = a_scanCode;
			}
			return true;
		}
		const auto captureUpScan = _captureUpScan.load();
		if (captureUpScan != kInvalidScanCode && a_scanCode == captureUpScan && !a_down) {
			_captureUpScan = kInvalidScanCode;
			return true;
		}

		// F12 opens WebView2 DevTools for the active menu in developer mode. Only raise
		// a flag here: renderer IPC belongs on the main tick.
		if (_config.devMode && a_vkCode == kVkF12) {
			if (a_down) {
				_devToolsRequested.store(true);
			}
			return true;
		}

		// Mod-hotkey dispatch: a key-down edge may fire mods'
		// key-typed bindings. The service self-suppresses while the overlay
		// captures input or a rebind is armed (belt and braces — the armed path
		// above already returned); fires queue here on the window thread and
		// deliver from Tick (DrainHotkeys). Does not consume: the game (and the
		// toggle/routing path below) still sees the key.
		if (a_down) {
			_hotkeys.OnKeyDown(a_scanCode);
		}

		// Toggle is handled before captured routing so it works in either state and
		// never also arrives as a plain page key. Captured Esc is the back action:
		// close the menu or delegate to a view that owns osfui.handleBack.
		const auto toggleKey = _toggleKey.load(std::memory_order_acquire);
		const bool captured = IsInputCaptured();
		const bool isToggle = toggleKey != kInvalidScanCode && a_scanCode == toggleKey;
		if (a_down) {
			if (isToggle) {
				EnqueuePresentationRequest(PresentationRequest::ToggleDefault);
			} else if (captured && a_scanCode == kScanEscape) {
				EnqueuePresentationRequest(PresentationRequest::Back);
			} else if (captured && _renderer) {
				_renderer->InjectKeyEvent(a_vkCode, true);
			} else if (Log::DevMode()) {
				REX::DEBUG("Runtime: OnGameWindowKey down (vk {}, scan {}) passed to the game", a_vkCode, a_scanCode);
			}
		} else {
			// Preserve the old router's release behavior: a toggle/back key-down is
			// framework-owned, but its release routes if the resulting menu is now
			// capturing. Chromium tolerates a release without a corresponding press.
			if (captured && _renderer) {
				_renderer->InjectKeyEvent(a_vkCode, false);
			} else if (Log::DevMode()) {
				REX::DEBUG("Runtime: OnGameWindowKey up (vk {}, scan {}) passed to the game", a_vkCode, a_scanCode);
			}
		}
		// Captured keys and both toggle transitions are swallowed before Starfield.
		return captured || isToggle;
	}


	bool Runtime::OnNativeAcceleratorKey(std::uint32_t a_vkCode, std::uint32_t a_scanCode, bool a_down)
	{
		const auto scan = static_cast<ScanCode>(a_scanCode);
		const auto toggleKey = _toggleKey.load(std::memory_order_acquire);
		const bool frameworkOwned =
			_captureArmed.load() ||
			(_captureUpScan.load() != kInvalidScanCode && scan == _captureUpScan.load()) ||
			(toggleKey != kInvalidScanCode && scan == toggleKey) ||
			(_config.devMode && a_vkCode == kVkF12) ||
			(a_vkCode == 0x1B && IsInputCaptured());
		return frameworkOwned && OnGameWindowKey(a_vkCode, scan, a_down);
	}

	void Runtime::OnGameWindowMouseAbsolute(int a_clientX, int a_clientY, int a_clientW, int a_clientH)
	{
		if (!IsInputCaptured() || !_renderer || a_clientW <= 0 || a_clientH <= 0) {
			return;
		}
		// The OS pointer moves in window-client space; the view is the same aspect
		// but height-capped (OnOutputResized), so scale through the client size.
		// Uniform scale keeps the pointer and the page's hit-testing aligned at
		// every resolution.
		const auto viewW = static_cast<float>(_viewWidth.load(std::memory_order_relaxed));
		const auto viewH = static_cast<float>(_viewHeight.load(std::memory_order_relaxed));
		_cursorX.store(std::clamp(static_cast<float>(a_clientX) * viewW /
			static_cast<float>(a_clientW), 0.0f, viewW - 1.0f), std::memory_order_relaxed);
		_cursorY.store(std::clamp(static_cast<float>(a_clientY) * viewH /
			static_cast<float>(a_clientH), 0.0f, viewH - 1.0f), std::memory_order_relaxed);
		QueueMouseMove();
	}

	void Runtime::QueueMouseMove()
	{
		// Raw-input packets arrive at the mouse's polling rate (500-1000 Hz);
		// a pipe write per packet is pure overhead when the page samples at
		// display refresh. Last writer wins — only the newest position
		// matters — and Tick flushes at most one InjectMouseMove per frame.
		// Coords are non-negative ints well under 2^31, so the packed value
		// can never equal the all-bits-set no-pending sentinel.
		const auto x = static_cast<std::uint32_t>(static_cast<int>(_cursorX.load(std::memory_order_relaxed)));
		const auto y = static_cast<std::uint32_t>(static_cast<int>(_cursorY.load(std::memory_order_relaxed)));
		_pendingMouseMove.store((static_cast<std::uint64_t>(x) << 32) | y);
		_mouseMovePackets.fetch_add(1, std::memory_order_relaxed);
	}

	void Runtime::OnGameWindowMouseButton(int a_button, bool a_down)
	{
		if (!IsInputCaptured() || !_renderer) {
			return;
		}
		_renderer->InjectMouseButton(
			static_cast<int>(_cursorX.load(std::memory_order_relaxed)),
			static_cast<int>(_cursorY.load(std::memory_order_relaxed)), a_button, a_down);
	}

	void Runtime::OnGameWindowMouseWheel(int a_wheelDelta)
	{
		if (!IsInputCaptured() || !_renderer) {
			return;
		}
		// Route at the current virtual cursor; the renderer forwards the raw
		// delta to the browser host's WebView2 WHEEL input, which performs the scroll.
		_renderer->InjectPhysicalMouseWheel(
			static_cast<int>(_cursorX.load(std::memory_order_relaxed)),
			static_cast<int>(_cursorY.load(std::memory_order_relaxed)), a_wheelDelta);
	}

	void Runtime::ReconcileFocusMenu()
	{
		// Main thread (Runtime::Tick). Drive the engine menu's open state toward
		// the active menu's capture policy. Pause is not wired through this menu's
		// flags
		// (the real pause flag, bit 1, would tie pause to capture instead of the
		// per-view pausesGame policy) — sim pause is ReconcileSimPause. Act only
		// on a change, to avoid per-frame queue spam.
		const bool wantOpen = _presentation.DesiredCapture();
		if (wantOpen != _focusMenuOpen) {
			_focusMenuOpen = wantOpen;
			_focusMenuMismatchSince = -1.0;  // fresh request: full grace window
			if (wantOpen) {
				FocusMenu::Open();
			} else {
				FocusMenu::Close();
				// Clear the gamepad routing queue/sticks.
				EngineInput::ResetSessionRouting();
				// Gamepad raw-passthrough is not reset here: it is a sticky
				// per-view property (_gamepadRawViews) that survives overlay
				// hide/show. Another menu opening can't inherit it, because
				// DrainEngineInput reads the active menu's flag each tick.
			}
			return;
		}

		// Watchdog: the request above is a fire-and-forget UI-queue message, so the
		// engine's admitted state must be checked to converge. A dropped kHide
		// leaves the engine in menu mode with the overlay gone — every control (Esc
		// included) dead until the process is killed (bug report 2026-07-20). A
		// dropped kShow is the milder mirror (game input under a capturing
		// overlay). The grace window covers queue latency (a frame or two) and
		// transition churn: a load-screen stack clear is followed by
		// MenuEventSink's CloseAll within a tick, re-entering the branch above.
		if (!FocusMenu::IsRegistered()) {
			return;
		}
		const bool engineOpen = FocusMenu::IsOpenInEngine();
		if (engineOpen == wantOpen) {
			_focusMenuMismatchSince = -1.0;
			return;
		}
		constexpr double kHealSeconds = 1.0;
		if (_focusMenuMismatchSince < 0.0) {
			_focusMenuMismatchSince = _uptime;
			return;
		}
		if (_uptime - _focusMenuMismatchSince < kHealSeconds) {
			return;
		}
		REX::WARN("FocusMenu: engine admitted state diverged from requested (want {}, engine {}) "
				  "for {:.1f}s; re-sending {} (watchdog)",
			wantOpen ? "open" : "closed", wantOpen ? "closed" : "open",
			_uptime - _focusMenuMismatchSince, wantOpen ? "kShow" : "kHide");
		_focusMenuMismatchSince = -1.0;  // re-arm: another full window before the next retry
		if (wantOpen) {
			FocusMenu::Open();
		} else {
			FocusMenu::Close();
		}
	}

	void Runtime::ReconcileSimPause()
	{
		// Main thread (Runtime::Tick), unconditional: the sim pause needs no
		// engine menu (UI::ModifyMenuPauseCounter; see Input/SimPause). Driven by
		// the active menu's pausesGame manifest policy (default true for menus).
		// Edge-triggered inside Apply.
		SimPause::Apply(_presentation.DesiredPause());
	}

	void Runtime::DrainEngineInput(double a_deltaSeconds)
	{
		if (!_renderer) {
			return;
		}
		const bool captured = IsInputCaptured();
		const auto active = _presentation.ActiveMenu();
		// Raw mode is the active menu's sticky flag — per view, so menu switches
		// can't leak one page's grant to another. The EngineInput global mirrors
		// it, keeping the mode-flip log in one place.
		const bool raw = active && _gamepadRawViews.contains(*active);
		EngineInput::SetRawMode(raw);
		// While capturing, the receiver thunks consume gamepad events after
		// recording them (status=kStop): the ControlLayer disable flags do not
		// gate thumbstick movement, so without this the player walks around
		// under the open overlay. Tracks capture, not visibility — an open HUD
		// (no capture) must leave the pad with the game.
		EngineInput::SetConsumeGamepad(captured);

		// Discrete down+up tap: a missed release can't leave a stuck key.
		const auto tap = [this](std::uint32_t a_vk) {
			_renderer->InjectKeyEvent(a_vk, true);
			_renderer->InjectKeyEvent(a_vk, false);
		};

		const auto routeButtonEdge = [&](const EngineInput::GamepadButtonEdge& e) {
			// Raw event for every edge — a page may own gamepad handling. Per-kind
			// nesting keeps extensions (e.g. a `pad` index) off the payload root.
			if (_bridge && active) {
				_bridge->Emit(*active, "ui.gamepad",
					nlohmann::json{ { "kind", "button" },
						{ "button", { { "id", e.idCode }, { "down", e.down } } } });
			}
			if (raw || !e.down) {
				return;  // raw mode = page owns it; else act on the press edge only
			}
			switch (e.idCode) {
			case XInputButton::kDPadUp:    tap(0x26); break;  // VK_UP
			case XInputButton::kDPadDown:  tap(0x28); break;  // VK_DOWN
			case XInputButton::kDPadLeft:  tap(0x25); break;  // VK_LEFT
			case XInputButton::kDPadRight: tap(0x27); break;  // VK_RIGHT
			case XInputButton::kA:         tap(0x0D); break;  // VK_RETURN — activate
			case XInputButton::kB:         EnqueuePresentationRequest(PresentationRequest::Back); break;  // back — delegate (osfui.handleBack) or close
			default: break;  // shoulders/thumbs/Start/Back -> raw event only
			}
		};

		// Starfield's Windows.Gaming.Input dispatch stops when WebView2 owns
		// foreground focus. Poll XInput only during that capturing interval and
		// keep draining the engine queue so no stale edges are replayed later.
		const bool directPad = captured && _nativeFocusGranted;
		XInputPoller::State directState{};
		EngineInput::GamepadButtonEdge e;
		if (directPad) {
			while (EngineInput::PollGamepadButton(e)) {}
			directState = XInputPoller::Poll();
			if (!_directPadActive) {
				// Baseline only: a held menu-open button must not activate the page.
				_directPadActive = true;
				_directPadButtons = directState.buttons;
			} else {
				const auto changed = _directPadButtons ^ directState.buttons;
				constexpr std::uint32_t masks[] = {
					XInputButton::kDPadUp, XInputButton::kDPadDown,
					XInputButton::kDPadLeft, XInputButton::kDPadRight,
					XInputButton::kStart, XInputButton::kBack,
					XInputButton::kLThumb, XInputButton::kRThumb,
					XInputButton::kLShoulder, XInputButton::kRShoulder,
					XInputButton::kA, XInputButton::kB,
					XInputButton::kX, XInputButton::kY,
				};
				for (const auto mask : masks) {
					if ((changed & mask) != 0) {
						routeButtonEdge({ mask, (directState.buttons & mask) != 0 });
					}
				}
				_directPadButtons = directState.buttons;
			}
		} else {
			_directPadActive = false;
			_directPadButtons = 0;
			// Next session re-picks the pad the player is actually holding.
			XInputPoller::ResetSlotLatch();
			while (EngineInput::PollGamepadButton(e)) {
				if (captured) {
					routeButtonEdge(e);
				}
			}
		}

		if (!captured) {
			// Reset routing timers so the next overlay open starts fresh.
			_padNavigation.Reset();
			_padScrollAccum = 0.0f;
			return;
		}

		const auto s = directPad ?
			EngineInput::GamepadSticks{ directState.lx, directState.ly, directState.rx, directState.ry } :
			EngineInput::GetSticks();
		constexpr float       kDeadzone = 0.25f;

		// Raw stick events, throttled to meaningful change, so a page can drive
		// e.g. camera orbit off the raw values.
		if (_bridge && active) {
			const float cur[4] = { s.lx, s.ly, s.rx, s.ry };
			bool        changed = false;
			for (int i = 0; i < 4; ++i) {
				changed = changed || std::fabs(cur[i] - _padLastSentSticks[i]) > 0.04f;
			}
			if (changed) {
				// Nested like the button case; triggers extend as axes.lt/rt.
				_bridge->Emit(*active, "ui.gamepad",
					nlohmann::json{ { "kind", "stick" },
						{ "axes", { { "lx", s.lx }, { "ly", s.ly }, { "rx", s.rx }, { "ry", s.ry } } } });
				for (int i = 0; i < 4; ++i) {
					_padLastSentSticks[i] = cur[i];
				}
			}
		}

		if (raw) {
			return;  // no default stick mapping in raw mode
		}

		// Left stick -> one arrow direction. The state machine prevents a normal
		// flick, release jitter, or diagonal input from skipping several controls;
		// a deliberate hold still repeats after a longer initial pause.
		const auto nav = _padNavigation.Update(s.lx, s.ly, _uptime);
		const std::uint32_t dirVk[4] = { 0x26, 0x28, 0x25, 0x27 };
		for (std::uint8_t i = 0; i < 4; ++i) {
			if ((nav & (1u << i)) != 0) {
				tap(dirVk[i]);
			}
		}

		// Right stick Y -> scroll. Fractional notches accumulate for
		// framerate-independent scrolling; +y (stick up) = wheel up.
		if (std::fabs(s.ry) > kDeadzone) {
			constexpr float kScrollNotchesPerSec = 8.0f;
			_padScrollAccum += s.ry * kScrollNotchesPerSec * static_cast<float>(a_deltaSeconds);
			if (const int notches = static_cast<int>(_padScrollAccum); notches != 0) {
				_renderer->InjectMouseWheel(
					static_cast<int>(_cursorX.load(std::memory_order_relaxed)),
					static_cast<int>(_cursorY.load(std::memory_order_relaxed)), notches * 120);
				_padScrollAccum -= static_cast<float>(notches);
			}
		} else {
			_padScrollAccum = 0.0f;
		}
	}

	void Runtime::ReconcileControlLayer()
	{
		// Main thread (Runtime::Tick). This is the only gate that stops
		// gamepad/XInput, so it tracks capture (not pause), or a gamepad drives the
		// game underneath a capturing menu. An open HUD (no capture) leaves
		// controls enabled. Apply edge-detects internally and retries until the
		// manager exists.
		ControlLayer::Apply(_presentation.DesiredCapture());
	}

	void Runtime::DrainKeyCapture()
	{
		const ScanCode scan = _capturedScan.exchange(kInvalidScanCode);
		if (scan == kInvalidScanCode) {
			return;  // nothing captured this tick
		}
		if (!_bridge || _captureView.empty()) {
			return;  // nobody to answer
		}
		// Escape cancels the rebind by contract; the Win keys are reserved
		// (Start-menu foot-gun); an unnameable scan can't be a binding.
		const bool reserved = scan == kScanLWin || scan == kScanRWin;
		const std::string name =
			(scan == kScanEscape || reserved) ? std::string{} : KeyName(scan);
		const bool cancelled = name.empty();
		// Tell the view which setting + the captured name; it echoes back a normal
		// settings.set, so the store persists and OnSettingChanged re-resolves.
		nlohmann::json payload{
			{ "mod", _captureMod },
			{ "key", _captureKey },
			{ "name", name },
			{ "cancelled", cancelled },
		};
		if (cancelled) {
			// Additive diagnosis so the UI can say WHY instead of silently
			// reverting: Esc = the player backed out; reserved = a key we
			// refuse to bind; unnameable = no identity for what was pressed.
			payload["reason"] = (scan == kScanEscape) ? "escape" :
			                    reserved              ? "reserved" :
			                                            "unnameable";
		} else {
			// Additive: the current layout's keycap for the captured key, so
			// the view can show "Ö" while committing the layout-independent
			// name. Display only — the echo (settings.set) carries `name`.
			payload["label"] = KeyLabelFor(name);
		}
		// Live warning during capture: which other key-typed
		// settings already sit on this key, so the UI warns before the view
		// commits. The store still holds this setting's old binding (the commit is
		// the view's echo), so exclude self. Informational, never blocking.
		if (!cancelled && _settings) {
			if (auto conflicts = _settings->Store().ConflictsFor(scan, _captureMod, _captureKey); !conflicts.empty()) {
				payload["conflicts"] = std::move(conflicts);
			}
		}
		// A one-shot happening, so it is an EVENT, not the arming request's
		// reply: `settings.captureKey` already settled in machine time with
		// "armed". Requests settle in machine time; human-time outcomes are
		// events (docs/mod-api-2.0-design.md, "User-paced flows settle fast").
		_bridge->Emit(_captureView, "settings.captured", payload);
		REX::DEBUG("Runtime: key capture -> {} (scan {:#04x}) ({}.{})",
			cancelled ? "(cancelled)" : name, scan, _captureMod, _captureKey);
		_captureView.clear();
		_captureMod.clear();
		_captureKey.clear();
		// The capture is answered; stop swallowing the captured key's release.
		// A letter/digit key never reaches the accelerator hook on key-up, so
		// without this the latch stays armed and eats that key's next release
		// in gameplay. The dangerous ups (Esc, the toggle key) are still owned
		// by their own conditions in OnNativeAcceleratorKey.
		_captureUpScan = kInvalidScanCode;
	}

	void Runtime::CancelArmedKeyCapture()
	{
		if (!_captureArmed.exchange(false)) {
			return;
		}
		_captureUpScan = kInvalidScanCode;
		// Close the capture out so the view's rebind affordance restores instead
		// of waiting forever on a keypress that can no longer arrive; same shape
		// as the Esc path in DrainKeyCapture.
		if (_bridge && !_captureView.empty()) {
			nlohmann::json payload{
				{ "mod", _captureMod },
				{ "key", _captureKey },
				{ "name", "" },
				{ "cancelled", true },
			};
			_bridge->Emit(_captureView, "settings.captured", payload);
		}
		REX::DEBUG("Runtime: armed key capture cancelled by menu close ({}.{})",
			_captureMod, _captureKey);
		_captureView.clear();
		_captureMod.clear();
		_captureKey.clear();
	}

	void Runtime::DrainHotkeys()
	{
		// Gameplay gate: a press while a game menu is up
		// (PauseMenu, inventory, dialogue, main menu, ...) must not fire. Checked
		// at delivery on the game thread via the engine's menu-mode discriminator
		// (MenuMode.h), lazily so idle ticks never touch RE::UI. Gated presses are
		// dropped, not deferred — replaying them on menu close would be worse.
		std::optional<bool> inGameMenu;
		_hotkeys.Drain([this, &inGameMenu](const std::string& a_mod, const std::string& a_key) {
			if (!inGameMenu) {
				inGameMenu = MenuMode::AnyGameMenuOpen();
			}
			if (*inGameMenu) {
				// INFO on purpose: rare (a bound key inside a menu/console), and the
				// decisive triage line for "my hotkey (didn't) fire" reports.
				REX::DEBUG("Runtime: hotkey {}.{} dropped (game menu open)", a_mod, a_key);
				return;
			}
			if (_settings) {
				const auto scope = _settings->Store().ScopeForHotkey(a_mod, a_key);
				if (scope.scoped) {
					const auto mode = _controlMap.CurrentMode();
					if (!_controlMap.Available() || !mode || !ModesOverlap(scope.modes, ModeBit(*mode))) {
						REX::DEBUG("Runtime: hotkey {}.{} dropped (scoped modes {:#x}, current mode {})",
							a_mod, a_key, scope.modes, mode ? GameplayModeName(*mode) : "unavailable");
						return;
					}
				}
			}
			// Delivery channels: native ABI subscribers (queued
			// here, invoked unlocked by BridgeApi::PumpMainThread later this
			// tick) and the web `ui.hotkey` event to every greeted view.
			API::BridgeApi::Get().Hotkeys().OnFired(a_mod, a_key);
			if (_settings) {
				_settings->PushHotkey(a_mod, a_key);
			}
			// Third channel: registered Papyrus callbacks,
			// queued onto the VM's async call stack.
			API::Papyrus::OnHotkey(a_mod, a_key);
			// Optional schema-owned GLOBAL callback. It is looked up from the
			// immutable schema at delivery instead of entering PapyrusApi's
			// session-scoped registration table, so save loads need no re-register.
			if (_settings) {
				if (const auto target = _settings->Store().GetHotkeyTarget(a_mod, a_key)) {
					const auto result = API::Papyrus::DispatchStaticHotkey(
						target->script, target->function, a_mod, a_key);
					if (result == API::Papyrus::StaticDispatchResult::kQueued) {
						_runtimeHealth.ResolveHotkeyTarget(a_mod, a_key);
					} else {
						const auto reason = result == API::Papyrus::StaticDispatchResult::kVmUnavailable ?
							"the Papyrus VM is unavailable" :
							"Papyrus rejected the call; the script may be missing, the function may be absent "
							"or non-GLOBAL, or its signature may not be (string, string)";
						_runtimeHealth.ReportHotkeyTargetFailure(
							a_mod, a_key, target->script, target->function, reason);
					}
				} else {
					_runtimeHealth.ResolveHotkeyTarget(a_mod, a_key);
				}
			}
			REX::DEBUG("Runtime: hotkey fired for {}.{}", a_mod, a_key);
		});
	}

	void Runtime::RegisterPlatformEndpoints(MessageBridge& a_bridge)
	{
		// The platform owns only framework-shell endpoints. Features register
		// their own; there is no generic "call native" escape hatch.
		//
		// The kind of each endpoint is chosen by ONE question: does the caller
		// need a completion? A dismissal cannot meaningfully fail, so `close` is
		// a send; opening a view by id can name one that does not exist,
		// so `menu.open` is a request. Reads-with-replay are neither — the four
		// registries a view used to `*.get` (settings, views, diagnostics, i18n)
		// are published as state instead, which is what makes them survive F5
		// with no lifecycle code in the view.
		a_bridge.RegisterSend("close", [this](const nlohmann::json&, MessageBridge& a_b) {
			if (a_b.CurrentSource() == kHandoffViewId && CancelPendingOpen()) {
				ApplyViewPresentationPolicy();
				return;
			}
			// Dismiss the calling view. Closing the active menu hides the menu
			// layer; a coexisting open HUD stays rendered.
			if (_presentation.Close(a_b.CurrentSource())) {
				ApplyViewPresentationPolicy();
			}
		});
		a_bridge.RegisterSend("setVisible", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string src(a_b.CurrentSource());
			const bool changed = Json::Get(a_p, "visible", false) ? _presentation.Open(src) : _presentation.Close(src);
			if (changed) {
				ApplyViewPresentationPolicy();
			}
		});
		// Open/close a view by id (defaults to the calling view). The frozen menu.*
		// names accept either kind; a view's kind is fixed by its manifest.
		const auto viewOpen = [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			std::string id = Json::Get(a_p, "view", "");
			if (id.empty()) {
				id = std::string(a_b.CurrentSource());
			}
			const auto* manifest = _views.Find(id);
			if (!manifest) {
				REX::WARN("Runtime: menu.open refused — '{}' was not discovered", id);
				a_b.Reject("unknown-view", "view was not discovered");
				return;
			}
			if (manifest->kind == ViewKind::Menu && manifest->capturesInput &&
				!_captureIntegrationAvailable) {
				REX::WARN("Runtime: menu.open refused — required input integration is unavailable");
				a_b.Reject("input-unavailable", "required input integration is unavailable");
				return;
			}
			// Use the same snapshot/instantiate/pump/open path as native RequestMenu so a
			// discovered view is instantiated while hidden on the next tick. The
			// reply means "accepted and queued", which is all the caller can act
			// on — the open itself lands on the next tick.
			EnqueueOpenView(std::move(id));
			a_b.Respond(nlohmann::json::object());
		};
		const auto viewClose = [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			std::string id = Json::Get(a_p, "view", "");
			if (id.empty()) {
				id = std::string(a_b.CurrentSource());
			}
			bool cancelled = false;
			if (_pendingViewOpen &&
				(_pendingViewOpen->target == id || id == kHandoffViewId)) {
				cancelled = CancelPendingOpen();
			}
			if (_presentation.Close(id)) {
				ApplyViewPresentationPolicy();
			} else if (cancelled) {
				ApplyViewPresentationPolicy();
			} else if (!_presentation.IsInstantiated(id)) {
				a_b.Reject("unknown-view", "view is not instantiated");
				return;
			}
			// Already closed = the desired state was reached.
			a_b.Respond(nlohmann::json::object());
		};
		// `hud.show`/`hud.hide` are gone: they were bound to these very lambdas,
		// so they were four names for two behaviors. A view's kind is fixed by
		// its manifest, not by the endpoint the page happened to pick.
		a_bridge.RegisterRequest("menu.open", viewOpen);
		a_bridge.RegisterRequest("menu.close", viewClose);
		a_bridge.RegisterSend("view.ready", [this](const nlohmann::json&, MessageBridge& a_b) {
			const std::string source(a_b.CurrentSource());
			const auto* manifest = _views.Find(source);
			if (!manifest || !manifest->permissions.nativeBridge) {
				// Unreachable in practice — a view without nativeBridge has no
				// bridge to send through — report it rather than answering.
				a_b.ReportProtocolFault(source, "forbidden", "view.ready requires nativeBridge");
				return;
			}
			_contentReadyViews.insert(source);
			REX::DEBUG("Runtime: view '{}' declared meaningful readiness", source);
		});
		a_bridge.RegisterSend("osfui.handoffRetry", [this](const nlohmann::json&, MessageBridge& a_b) {
			if (a_b.CurrentSource() != kHandoffViewId) {
				a_b.ReportProtocolFault(a_b.CurrentSource(), "forbidden", "osfui.handoffRetry is a platform action");
				return;
			}
			RetryPendingOpen();
		});
		a_bridge.RegisterRequest("setViewHidden", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			// Show/hide one instantiated view by id, independent of the overlay toggle.
			// Omitting "view" targets the calling view (self-hide).
			std::string id = Json::Get(a_p, "view", "");
			if (id.empty()) {
				id = std::string(a_b.CurrentSource());
			}
			if (!SetViewHidden(id, Json::Get(a_p, "hidden", false))) {
				a_b.Reject("unknown-view", "not an instantiated view");
				return;
			}
			a_b.Respond(nlohmann::json::object());
		});
		// `views.get`, `i18n.get`, `settings.get` and `diagnostics.get` are GONE.
		// Each was a request with an invisible side effect — it subscribed the
		// caller to future pushes — which is the definition of state, not of a
		// read. They are published as the platform state keys osfui/views,
		// osfui/i18n, osfui/settings and osfui/diagnostics instead
		// (PublishPlatformState below), so a view renders from
		// `osfui.state.on(...)` with no read roundtrip and no re-request after a
		// reload.
		// Arm key-rebind capture. The REQUEST settles in machine time — "armed",
		// or a typed refusal — and the human-time outcome arrives later as the
		// `settings.captured` EVENT. A request left pending on a person pressing
		// a key is the wrong shape: it fights the client's own timeout and makes
		// "waiting for you" indistinguishable from "the endpoint handler died".
		// Any schema-declared `type:"key"` setting is rebindable — the schema
		// gates the capture, not an allowlist.
		// Main thread; OnGameWindowKey (window thread) reads the armed flag.
		a_bridge.RegisterRequest("settings.captureKey", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			const auto requestedMod = Json::Get(a_p, "mod", "");
			// A capture ends in a settings write, so it carries the same authority
			// requirement (Ids::ResolveWritableMod): only the built-in Mod Settings view
			// and Keybindings view may rebind another mod's keys.
			const auto allowedMod = Ids::ResolveWritableMod(a_b.CurrentSource(), requestedMod);
			if (!allowedMod) {
				REX::WARN("Runtime: [content] view '{}' refused settings.captureKey for '{}' (not its own mod)",
					a_b.CurrentSource(), requestedMod);
				a_b.Reject("forbidden", "a view may only rebind its own mod's keys");
				return;
			}
			const std::string mod(*allowedMod);
			const std::string key = Json::Get(a_p, "key", "");
			// One capture at a time: a second arm while one is in progress is refused
			// visibly rather than silently clobbering the first view's pending
			// capture.
			if (_captureArmed.load()) {
				REX::WARN("Runtime: settings.captureKey rejected — a capture is already in progress ({}.{})",
					_captureMod, _captureKey);
				a_b.Reject("capture-busy", "a key capture is already in progress");
				return;
			}
			if (!_settings || _settings->Store().GetSettingType(mod, key) != "key") {
				REX::WARN("Runtime: settings.captureKey rejected — '{}.{}' is not a key-typed setting",
					mod.substr(0, 64), key.substr(0, 64));
				a_b.Reject("not-rebindable", "only a key-typed setting can be rebound");
				return;
			}
			// Freshen keycap labels before the press: the captured answer (and
			// the view's current display) reads them, and this catches a layout
			// switch made while the WebView held focus (WM_INPUTLANGCHANGE
			// went to Chromium's window, not the game's).
			RefreshKeyboardLabels("capture arm");
			_captureView = std::string(a_b.CurrentSource());
			_captureMod = mod;
			_captureKey = key;
			_captureArmed.store(true);
			REX::DEBUG("Runtime: armed key capture for {}.{} (from view '{}')", mod, key, _captureView);
			// Settled: capture is armed. The captured key (or the cancellation)
			// follows as a `settings.captured` event, however much later.
			a_b.Respond(nlohmann::json{ { "armed", true }, { "mod", mod }, { "key", key } });
		});
		// The two fixed endpoints behind `osfui.papyrus.*`. The mod id comes from
		// the source view id, never the payload, so a view cannot reach into
		// another mod's listeners.
		//
		// Non-string arg elements are coerced here so a view can send
		// `args: [1, 7]` without stringifying — Papyrus's lack of a modulo
		// operator made packing several small ints into one string genuinely
		// painful, which is why the list form exists at all.
		const auto papyrusArgs = [](const nlohmann::json& a_p) {
			std::vector<std::string> args;
			const auto* list = Json::GetArray(a_p, "args");
			if (!list) {
				return args;
			}
			args.reserve(list->size());
			for (const auto& e : *list) {
				if (e.is_string()) {
					args.push_back(e.get<std::string>());
				} else if (e.is_number_unsigned()) {
					// Before is_number_integer(), which is true for unsigned too:
					// as int64 a > INT64_MAX literal stringifies as negative.
					args.push_back(std::to_string(e.get<std::uint64_t>()));
				} else if (e.is_number_integer()) {
					args.push_back(std::to_string(e.get<std::int64_t>()));
				} else if (e.is_number()) {
					args.push_back(std::to_string(e.get<double>()));
				} else if (e.is_boolean()) {
					args.emplace_back(e.get<bool>() ? "true" : "false");
				} else {
					args.emplace_back();  // null/object/array element -> ""
				}
			}
			return args;
		};
		a_bridge.RegisterSend("papyrus.call", [](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string source(a_b.CurrentSource());
			// Validation and marshalling live in PapyrusCall so the native unit tests
			// can reach them; this endpoint owns only the transport half.
			const auto call = PapyrusCall::Parse(a_p);
			if (!call.ok) {
				a_b.ReportProtocolFault(source, call.code, call.message);
				return;
			}
			if (API::Papyrus::DispatchStaticFunction(call.script, call.function, call.args) !=
				API::Papyrus::StaticDispatchResult::kQueued) {
				a_b.ReportProtocolFault(source, "papyrus-unavailable", "Papyrus could not queue the GLOBAL function");
			}
		});
		a_bridge.RegisterSend("papyrus.send", [papyrusArgs](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string source(a_b.CurrentSource());
			const std::string mod{ Ids::ModOf(source) };
			const std::string name = Json::Get(a_p, "name", "");
			if (name.empty()) {
				a_b.ReportProtocolFault(source, "invalid-request", "papyrus.send requires a non-empty 'name'");
				return;
			}
			API::Papyrus::OnViewAction(mod, name, papyrusArgs(a_p));
		});
		a_bridge.RegisterRequest("papyrus.request", [papyrusArgs](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string source(a_b.CurrentSource());
			const std::string mod{ Ids::ModOf(source) };
			const std::string name = Json::Get(a_p, "name", "");
			if (mod.empty() || name.empty() || name.size() > 64) {
				a_b.Reject("invalid-request", "name must be a non-empty string of at most 64 characters");
				return;
			}
			// Defer first: the bridge's token is what settles this later, and
			// the Papyrus side has to be handed it up front. The page's own
			// request id would collide across documents.
			const auto token = a_b.Defer();
			if (!API::Papyrus::OnViewRequest(mod, name, papyrusArgs(a_p), source, token)) {
				a_b.RejectTo(token, "papyrus-unavailable", "no Papyrus request listener is available");
				return;
			}
			// The script settles it later through ReplyView*/RejectViewRequest.
		});
		a_bridge.RegisterSend("log", [](const nlohmann::json& a_p, MessageBridge&) {
			// Untrusted content: bound the length so JS cannot flood the log.
			REX::DEBUG("MessageBridge: [web] {}", Json::Get(a_p, "text", "").substr(0, 512));
		});
		a_bridge.RegisterRequest("ping", [](const nlohmann::json&, MessageBridge& a_b) {
			a_b.Respond(nlohmann::json::object());
		});
		a_bridge.RegisterSend("osfui.gamepadRaw", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			// A page that wants to own the gamepad (e.g. stick-driven camera orbit)
			// sets this to suppress the default nav/scroll mapping and handle raw
			// `ui.gamepad` events itself. Sticky per view: survives overlay
			// hide/show, clears on page reload or view destroy. DrainEngineInput
			// applies the active menu's flag each tick.
			const std::string src(a_b.CurrentSource());
			if (src.empty()) {
				return;
			}
			if (Json::Get(a_p, "raw", false)) {
				_gamepadRawViews.insert(src);
			} else {
				_gamepadRawViews.erase(src);
			}
		});
		// `osfui.textFocus` is gone. It was registered as a no-op purely so a
		// pre-session-focus view would not trip the legacy `unknown-command` error; an unknown
		// send is now a dev-only debug event, so the placeholder bought nothing.
		a_bridge.RegisterRequest("osfui.openModPage", [](const nlohmann::json&, MessageBridge& a_b) {
			// "Update OSF UI" affordances in views (e.g. OSF Animation's status-line
			// UPDATE badge): open OSF UI's own Nexus page in the SYSTEM browser —
			// the overlay itself must never navigate, and the URL is a compile-time
			// constant precisely so page content cannot steer the shell (the
			// payload carries nothing). Behind a fullscreen game the browser opens
			// unfocused; alt-tab exposes it.
			if (Platform::OpenSystemBrowser(kNexusPageURLW)) {
				// INFO for the same reason as osfui.openLogFolder below.
				REX::INFO("Runtime: osfui.openModPage -> {}", kNexusPageURL);
				a_b.Respond(nlohmann::json::object());
			} else {
				REX::WARN("Runtime: osfui.openModPage — the shell refused to open {}", kNexusPageURL);
				a_b.Reject("shell-failed", "could not open the system browser");
			}
		});
		a_bridge.RegisterRequest("osfui.openLogFolder", [](const nlohmann::json&, MessageBridge& a_b) {
			// System Health's "Open log folder" action, introduced in web bridge protocol 1.4. The twin
			// of osfui.openModPage and held to the same rule: the target is
			// DERIVED NATIVELY (Paths::LogDir()) and the payload carries nothing,
			// so no amount of page content can turn this into "open an arbitrary
			// folder" — let alone "run an arbitrary thing". Platform::OpenFolder
			// additionally refuses anything that is not an existing directory.
			const auto folder = Paths::LogDir();
			if (folder.empty()) {
				REX::WARN("Runtime: osfui.openLogFolder — could not resolve the Documents folder");
				a_b.Reject("no-log-folder", "could not resolve the log folder");
				return;
			}
			if (Platform::OpenFolder(folder)) {
				// INFO, not DEBUG: the window opens behind a fullscreen game, so a
				// working button and a dead one look identical to the player. At
				// the default log level this line is the only way to tell them
				// apart — it cost a bug report once already.
				REX::INFO("Runtime: osfui.openLogFolder -> {}", folder.string());
				a_b.Respond(nlohmann::json::object());
			} else {
				REX::WARN("Runtime: osfui.openLogFolder — the shell refused to open {}", folder.string());
				a_b.Reject("shell-failed", "could not open the log folder");
			}
		});
		a_bridge.RegisterRequest("osfui.setViewAutoStart", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			// Startup policy is player intent: only the built-in Mod Settings view may
			// change it — the same exact-id gate used by Mod Settings-only platform requests.
			if (a_b.CurrentSource() != kSettingsViewId) {
				a_b.Reject("forbidden", "view auto-start is set from OSF UI's built-in settings view");
				return;
			}
			const auto view = Json::Get(a_p, "view", "");
			const auto enabled = a_p.find("enabled");
			if (view.empty() || enabled == a_p.end() || !enabled->is_boolean()) {
				a_b.Reject("invalid-payload", "expected { view: string, enabled: boolean }");
				return;
			}
			const auto* manifest = _views.Find(view);
			if (!manifest) {
				a_b.Reject("unknown-view", "not a discovered view");
				return;
			}
			if (!HudAutoStartEligible(*manifest)) {
				a_b.Reject("not-configurable",
					"auto-start is settable only for catalog-visible HUDs");
				return;
			}
			if (!_viewPolicy.SetHudAutoStart(view, enabled->get<bool>())) {
				a_b.Reject("persistence-failed",
					"the choice could not be saved, so it was not applied");
				return;
			}
			// Deliberately takes effect at the next launch only — no view is
			// opened or torn down here. The rebroadcast carries the new effective
			// policy in the osfui/views state key.
			REX::INFO("Runtime: HUD '{}' auto-start set to {} (next launch)",
				view, enabled->get<bool>());
			BroadcastViewsData();
			a_b.Respond(nlohmann::json::object());
		});
		a_bridge.RegisterSend("osfui.handleBack", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			// A page that owns back navigation (e.g. a sub-menu whose Esc should
			// return to Mod Settings, not dismiss the overlay) sets this; while it is
			// the active menu, Esc / pad-B arrive as a synthetic Escape
			// keydown/keyup instead of closing the active menu. Same lifecycle as
			// osfui.gamepadRaw. The toggle key still closes natively, so this
			// cannot strand the user.
			const std::string src(a_b.CurrentSource());
			if (src.empty()) {
				return;
			}
			if (Json::Get(a_p, "handle", false)) {
				_backOwnerViews.insert(src);
			} else {
				_backOwnerViews.erase(src);
			}
		});

		// Read-only game data: bridge handlers dispatch from main-thread Tick, so
		// the in-game Calendar fields are read on their owning thread.
		a_bridge.RegisterRequest("game.get", [](const nlohmann::json&, MessageBridge& a_b) {
			nlohmann::json calendar = nlohmann::json::object();
			if (const auto* cal = RE::Calendar::GetSingleton()) {
				calendar["available"] = true;
				calendar["day"] = cal->GetDay();
				calendar["month"] = cal->GetMonth();
				calendar["year"] = cal->GetYear();
				calendar["hour"] = cal->GetHour();
				calendar["daysPassed"] = cal->GetDaysPassedExact();
			} else {
				calendar["available"] = false;
			}
			a_b.Respond(nlohmann::json{ { "calendar", std::move(calendar) } });
		});
	}

	void Runtime::OnSettingChanged(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value)
	{
		// Only the framework's own knobs (mod "osfui"); other mods' settings are
		// theirs to react to. Invoked from main-thread Tick as settings changes
		// dispatch, plus once per value at startup via NotifyAll, so persisted
		// choices apply on boot.
		if (a_modId != "osfui") {
			return;
		}
		// Toggle key rebind: re-resolve and publish it to the window thread. An
		// unresolvable name keeps the working key rather than disabling the toggle.
		if (a_key == "toggleKey" && a_value.is_string()) {
			const auto name = a_value.get<std::string>();
			const auto scan = ResolveKeyName(name);
			if (scan == kInvalidScanCode) {
				REX::WARN("Runtime: setting osfui.toggleKey '{}' is not a resolvable key; keeping '{}'", name, _config.toggleKey);
				return;
			}
			_config.toggleKey = name;
			_toggleKey.store(scan, std::memory_order_release);
			REX::INFO("Runtime: setting osfui.toggleKey -> {} (scan {:#x})", name, scan);
		}
		// Pause-menu entry (Mod Settings-owned). The Scaleform inject runs per pause-menu
		// open (MainThreadMenuPump gates Reconcile on this flag), so the change
		// applies the next time the menu opens.
		else if (a_key == "pauseMenuEntry" && a_value.is_boolean()) {
			_config.pauseMenuEntry = a_value.get<bool>();
			PauseMenuEntry::SetEnabled(_config.pauseMenuEntry);
			REX::DEBUG("Runtime: setting osfui.pauseMenuEntry -> {} (applies the next time the pause menu opens)", _config.pauseMenuEntry);
		}
		// Game-binding conflict warnings (Mod Settings-owned). Lazy build / clear.
		else if (a_key == "vanillaKeyConflicts" && a_value.is_boolean()) {
			_config.gameBindingWarnings = a_value.get<bool>();
			ApplyGameBindingConflictWarnings(_config.gameBindingWarnings);
		}
		else if (a_key == "language" && a_value.is_string()) {
			const auto requested = a_value.get<std::string>();
			const auto documents = Platform::GetDocumentsPath();
			const auto locale = requested == "auto"
				? LocalizationService::DetectGameLocale(documents.empty() ? std::filesystem::path{} : documents / "My Games" / "Starfield")
				: LocalizationService::NormalizeLocale(requested);
			if (_localization.SetLocale(locale)) {
				RefreshLocalizedData();
			}
		}
	}

	void Runtime::NotifyKeyboardLayoutChanged()
	{
		_keyboardLayoutChanged.store(true);
	}

	void Runtime::RefreshKeyboardLabels(const char* a_reason)
	{
		if (!_settings) {
			return;
		}
		auto labels = BuildKeyLabels(
			Platform::MakeKeyLabelSource(OverlayInputHook::GameWindowHandle()),
			[this](std::string_view a_address, std::string_view a_english) {
				return _localization.Resolve("osfui", a_address, a_english);
			});
		if (labels == _keyLabels) {
			return;  // same layout + locale: nothing to publish
		}
		_keyLabels = std::move(labels);
		_settings->Store().SetKeyboardLabels(_keyLabels.layout, _keyLabels.labels);
		_settings->BroadcastData();
		REX::DEBUG("Runtime: keyboard labels rebuilt ({}; layout '{}', {} keys)",
			a_reason, _keyLabels.layout, _keyLabels.labels.size());
	}

	std::string Runtime::KeyLabelFor(std::string_view a_name) const
	{
		for (const auto& [name, label] : _keyLabels.labels) {
			if (name == a_name) {
				return label;
			}
		}
		return std::string(a_name);
	}

	void Runtime::RefreshLocalizedData()
	{
		// Non-printing key labels resolve through chrome.keys.* addresses, so
		// a locale/catalog change re-derives them too.
		RefreshKeyboardLabels("locale change");
		PauseMenuEntry::Configure(
			_localization.Resolve("osfui", "chrome.pauseMenuEntry", _config.pauseMenuEntryLabel),
			_config.pauseMenuEntryView);
		if (_settings) {
			_settings->Store().InvalidateLocalizedData();
			if (_controlMap.RefreshLabels(/*localizationChanged*/ true)) {
				SyncLiveControlMapBindings();
				PublishPlatformState("keybindings");
			} else if (_controlMap.Initialized() && !_controlMap.Available()) {
				SyncLiveControlMapBindings();
				SyncLiveControlMapHealth();
				PublishPlatformState("keybindings");
				PublishPlatformState("input-context");
			} else {
				_settings->BroadcastData();
			}
		}
		BroadcastViewsData();
		PublishPlatformState("i18n");
	}

	void Runtime::ApplyGameBindingConflictWarnings(bool a_enabled)
	{
		if (!_settings) return;
		if (_settings->Store().SetGameBindingWarningsEnabled(a_enabled)) {
			_settings->BroadcastData();
		}
		REX::DEBUG("Runtime: game-binding conflict warnings {} (read-only game-binding catalog remains available)", a_enabled ? "enabled" : "disabled");
	}

    void Runtime::SyncLiveControlMapBindings()
	{
		if (!_settings) return;
		std::vector<SettingsStore::GameBinding> bindings;
		if (_controlMap.Available()) {
			const auto owner = _localization.Resolve("osfui", "gameBindings.owner", "Starfield");
			bindings.reserve(_controlMap.ConflictBindings().size());
			for (const auto& binding : _controlMap.ConflictBindings()) {
				bindings.push_back({
					binding.event, owner + " (" + binding.title + ")", binding.code,
					KeyName(static_cast<ScanCode>(binding.code)), binding.engineInputContextName, binding.slot,
					binding.classification, binding.definiteModes, binding.possibleModes,
				});
			}
		}
		auto& store = _settings->Store();
		const bool bindingsChanged = store.SetGameBindings(std::move(bindings));
		const bool warningChanged = store.SetGameBindingWarningsEnabled(_config.gameBindingWarnings);
		if (bindingsChanged || warningChanged) {
			_settings->BroadcastData();
		}
	}

	void Runtime::SyncLiveControlMapHealth()
	{
		if (!_healthRegistry || !_controlMap.Initialized()) return;
		constexpr std::string_view id{ "input.control-map-unavailable" };
		if (_controlMap.Available()) {
			_healthRegistry->Resolve(id, _uptime);
		} else {
			_healthRegistry->Upsert({
				.id = std::string(id), .code = std::string(id),
				.severity = HealthRegistry::Severity::Warning,
				.source = "input", .subject = "Starfield ControlMap",
				.context = { { "gameVersion", _controlMap.GameVersion() }, { "reason", _controlMap.FailureReason() } },
			}, _uptime);
		}
		_healthRegistry->Broadcast();
	}

	void Runtime::OnOutputResized(std::uint32_t a_width, std::uint32_t a_height)
	{
		if (a_width == 0 || a_height == 0 || !_renderer) {
			return;
		}
		// Match the view's aspect to the screen, height-capped so rasterization
		// stays bounded on 4K+ (the page is responsive, so any size lays out).
		// Equal aspect makes the compositor's fill-the-backbuffer draw a uniform
		// scale, i.e. no distortion.
		constexpr std::uint32_t kMaxViewHeight = 1440;
		const auto viewHeight = (std::min)(a_height, kMaxViewHeight);
		const auto viewWidth = static_cast<std::uint32_t>(
			std::lround(static_cast<double>(a_width) * viewHeight / a_height));

		if (viewWidth == _viewWidth.load() && viewHeight == _viewHeight.load()) {
			return;
		}

		_viewWidth.store(viewWidth);
		_viewHeight.store(viewHeight);
		_renderer->Resize(viewWidth, viewHeight);
		REX::DEBUG("Runtime: output {}x{} -> view resized to {}x{} (aspect-correct)",
			a_width, a_height, viewWidth, viewHeight);
	}

	void Runtime::SubmitFrameIfVisible()
	{
		if (!_initialized || !IsVisible() || !_renderer || !_compositor) {
			return;
		}

		const auto frame = _renderer->Render();
		std::optional<ViewRevealGate::FrameObservation> observation;
		if (frame) {
			observation = ViewRevealGate::FrameObservation{
				.index = frame->frameIndex,
				.outputSizeKnown = _compositor->IsOutputSizeKnown(),
				.matchesExpectedSize = frame->width == _viewWidth.load() &&
					frame->height == _viewHeight.load(),
			};
		}

		const auto decision = m_viewReveal.Observe(observation, _uptime);
		if (decision.submitFrame && frame) {
			// A fresh held frame is submitted while still hidden; this also starts
			// the compositor's lazy seam setup so output dimensions can arrive.
			_compositor->Submit(*frame);
		}
		if (decision.reveal) {
			_compositor->SetVisible(true);  // the cached frame is fresh and output-sized
			return;
		}
		if (!decision.timedOut) {
			return;
		}

		const auto active = _presentation.ActiveMenu().value_or("<none>");
		REX::ERROR("Runtime: overlay reveal for '{}' produced no presentable frame in {:.1f}s — "
				   "closing it and releasing input/pause state",
			active, decision.heldSeconds);
		CancelPendingOpen();
		_presentation.CloseAll();
		ApplyViewPresentationPolicy();
		// The timeout fires after this tick's normal policy reconciliation.
		// Release every engine-owned edge now instead of trapping input/pause
		// until another main-thread task happens to run.
		ReconcileFocusMenu();
		ReconcileControlLayer();
		ReconcileSimPause();
		FreeCursor::Apply(false);
	}

}
