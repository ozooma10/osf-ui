#include "Runtime/Runtime.h"

#include <cmath>
#include <limits>

#include "API/BridgeApi.h"
#include "API/PapyrusApi.h"
#include "Compat/V1/Papyrus.h"
#include "Composite/UiPass.h"
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
#include "Core/Ids.h"
#include "Render/WebView2HostWebRenderer.h"
#include "Views/BuiltinViewIds.h"

namespace OSFUI
{
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

		Log::SetDevMode(false);
		return true;
	}

	void Runtime::LoadStartupContent()
	{
		const auto documents = Platform::GetDocumentsPath();
		const auto starfieldDir = documents.empty() ? std::filesystem::path{} : documents / "My Games" / "Starfield";

		_localization.Load(Paths::DataDir() / "l10n", LocalizationService::DetectGameLocale(starfieldDir));

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

		const auto* view = _views.Find(Ids::kSettingsViewId);
		const auto initialWidth = view ? view->width : kDefaultViewWidth;
		const auto initialHeight = view ? view->height : kDefaultViewHeight;

		_viewWidth.store(initialWidth);
		_viewHeight.store(initialHeight);
		_cursorX = initialWidth * 0.5f;
		_cursorY = initialHeight * 0.5f;

		WebView2HostConfig rendererConfig{
			.width = initialWidth,
			.height = initialHeight,
			.devMode = false,
			.dataDir = Paths::DataDir(),
		};

		if (!_renderer->Initialize(rendererConfig)) {
			REX::ERROR("Runtime: WebView2 renderer failed to initialize");
			return false;
		}

		REX::INFO("Runtime: renderer = webview2");
		return true;
	}

	void Runtime::WireRendererLifecycleCallbacks()
	{
		_renderer->SetLoadHandler([this](const WebView2HostWebRenderer::LoadEvent& a_e) {
			OnViewLoad(a_e.viewId, a_e.failed, a_e.url, a_e.description, a_e.errorCode);
		});

		_renderer->SetFailureHandler([this](const WebView2HostWebRenderer::FailureEvent& a_e) {
			OnRendererFailure(a_e);
		});

		_renderer->SetHealthHandler([this](const WebView2HostWebRenderer::HealthEvent& a_e) {
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
		REX::INFO("Runtime: compositor = d3d12");
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
		std::size_t instantiated = 0;
		for (const auto& manifest : _views.All()) {
			if (manifest.kind != ViewKind::Hud) {
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
		REX::INFO("Runtime: instantiated {} auto-start HUD view(s); default menu = '{}'", instantiated, Ids::kSettingsViewId);
		if (!_views.Find(Ids::kSettingsViewId)) {
			REX::WARN("Runtime: default view '{}' was not discovered; the toggle key will have nothing to open", Ids::kSettingsViewId);
		}
    }

    void Runtime::ConfigureInputRouting()
    {
		const auto toggleKey = ResolveKeyName(Ids::kToggleKey);
		_toggleKey.store(toggleKey, std::memory_order_release);

		if (toggleKey != kInvalidScanCode) {
			REX::INFO("Runtime: toggleKey '{}' resolved to scan code {:#x}", Ids::kToggleKey, toggleKey);
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

		//@TODO: DEV RELOAD WORKER
		// _devViewReload = std::make_unique<DevViewReloadWorker>(
		// 	Paths::ViewsDir(), [this](std::string_view a_id) {
		// 		return _renderer && _renderer->RefreshViewFiles(a_id);
		// 	});

		_initialized = true;
		// Push the initial policy derived from whatever is open (incl. nothing).
		ApplyViewPresentationPolicy();
		REX::INFO("Runtime: initialized (visible={})", _visible.load());

		return true;
	}

	bool Runtime::InstallOverlayDrawPath()
	{
		if (!_compositor) {
			return false;
		}
		const bool installed = UiPass::Install();
		_overlayDrawAvailable.store(installed, std::memory_order_release);
		_compositor->SetUiPassDrawEnabled(installed);
		if (!installed) {
			REX::ERROR("Runtime: the Scaleform UI pass could not be hooked — menu opens will be "
					   "refused this session so OSF UI cannot capture input without a draw path. "
					   "See the [UiPass] lines above.");
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

	void Runtime::EnqueuePresentationRequest(ViewPresentationRequest a_req)
	{
		m_viewRequests.Enqueue(a_req);
	}

	void Runtime::EnqueueOpenView(std::string a_viewId)
	{
		m_viewRequests.EnqueueOpen(std::move(a_viewId));
	}


	Runtime::PendingPresentationWork Runtime::TakePresentationRequests()
	{
		auto queued = m_viewRequests.Take();
		PendingPresentationWork work;
		work.local = std::move(queued.presentation);
		work.openViews = std::move(queued.openViews);
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
			std::ranges::find(a_work.local, ViewPresentationRequest::ToggleDefault) != a_work.local.end()) {
			prepare(Ids::kSettingsViewId, "for the default-menu toggle");
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
			case ViewPresentationRequest::ToggleDefault:
				if (_pendingViewOpen) {
					CancelPendingOpen();
				} else if (_presentation.ActiveMenu()) {
					_presentation.CloseActiveMenu();
				} else {
					BeginViewOpen(Ids::kSettingsViewId);
				}
				break;
			case ViewPresentationRequest::Back: {
				// Esc / pad-B. A back-owning active menu (osfui.handleBack) gets
				// the action delegated as a synthetic Escape tap and decides for
				// itself — navigate elsewhere, peel an inner panel, or send
				// `close`. Everyone else closes the active menu (single-menu policy:
				// that hides the overlay). The toggle key never delegates, so a
				// broken page cannot strand the user.
				const auto active = _presentation.ActiveMenu();
				if (_pendingViewOpen) {
					CancelPendingOpen();
				} else if (active && m_viewInputGrants.OwnsBackAction(*active) && _renderer) {
					constexpr std::uint32_t kVkEscape = 0x1B;
					_renderer->InjectKeyEvent(kVkEscape, true);
					_renderer->InjectKeyEvent(kVkEscape, false);
				} else {
					_presentation.CloseActiveMenu();
				}
				break;
			}
			case ViewPresentationRequest::CloseAll:
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
				if (_pendingViewOpen && *_pendingViewOpen == r.view) {
					CancelPendingOpen();
				}
				_presentation.Close(r.view);
			}
		}
		ApplyViewPresentationPolicy();
	}

	bool Runtime::OverlayCanDraw() const
	{
		return _overlayDrawAvailable.load(std::memory_order_acquire) && UiPass::DrawEnabled();
	}

	bool Runtime::BeginViewOpen(std::string_view a_id)
	{
		// Both halves: Install() only proves the vtable hooks were taken, while
		// the command-list hooks are self-tested lazily on a render worker and
		// can disable the draw path afterwards. Gating on the install alone admits an
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
		if (!manifest || manifest->kind == ViewKind::Hud) {
			CancelPendingOpen();
			return _presentation.Open(a_id);
		}

		const auto loadState = m_viewLoads.GetState(a_id);
		if (loadState == ViewLoadState::Finished) {
			CancelPendingOpen();
			return _presentation.Open(a_id);
		}
		if (_pendingViewOpen && *_pendingViewOpen == a_id) {
			return false;
		}

		CancelPendingOpen();
		_pendingViewOpen = std::string(a_id);
		REX::DEBUG("Runtime: holding first open of '{}' until its main-frame load succeeds", a_id);
		return true;
	}

	bool Runtime::CancelPendingOpen()
	{
		if (!_pendingViewOpen) {
			return false;
		}
		const auto target = std::move(*_pendingViewOpen);
		_pendingViewOpen.reset();
		REX::DEBUG("Runtime: cancelled pending open of '{}'", target);
		return true;
	}

	void Runtime::DrivePendingOpen()
	{
		if (!_pendingViewOpen) {
			return;
		}
		const auto target = *_pendingViewOpen;
		const auto* manifest = _views.Find(target);
		if (!manifest || !_presentation.IsInstantiated(target)) {
			REX::WARN("Runtime: cancelling pending open of '{}' because the view is no longer available", target);
			CancelPendingOpen();
			return;
		}

		if (m_viewLoads.GetState(target) != ViewLoadState::Finished) {
			return;
		}

		_presentation.Open(target);
		_pendingViewOpen.reset();
		REX::DEBUG("Runtime: main-frame load completed; opening '{}'", target);
		ApplyViewPresentationPolicy();
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
				BeginViewOpen(id);
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
		// available. This central guard remains the final safety net for every
		// menu-opening path after BeginViewOpen performs its admission checks.
		if (_presentation.DesiredCapture() && !_captureIntegrationAvailable) {
			REX::WARN("Runtime: closing a requested menu because required input integration is unavailable");
			CancelPendingOpen();
			_presentation.CloseActiveMenu();
		}
		// Per-view hidden + composite z, derived from the band order: HUDs
		// by `order` beneath the one active menu.
		for (const auto& layer : _presentation.DesiredLayers()) {
			_renderer->SetViewHidden(layer.id, layer.hidden);
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

		m_viewRecovery.ClearAll();
		m_viewInputGrants.ResetAll();
		_pendingMouseMove.store(kNoPendingMouseMove);
		m_viewReveal.Reset();
		_nativeFocusGranted = false;
		std::size_t reloaded = 0;
		for (const auto& manifest : _views.All()) {
			if (!_presentation.IsInstantiated(manifest.id)) {
				continue;
			}
			m_viewLoads.BeginLoad(manifest.id);
			_renderer->CreateOrNavigateView(manifest);
			if (manifest.permissions.nativeBridge) {
				// RestartAfterFailure discarded messages addressed to the dead
				// documents. Each replacement greets the bridge on load and is
				// replayed then; all this has to do is re-arm its gate.
				_bridge->OnViewCreated(manifest.id, IsPre2Target(manifest.targetVersion));
			}
			++reloaded;
		}

		_renderer->Resize(_viewWidth.load(), _viewHeight.load());
		_renderer->SetAcceleratorKeys(_toggleKey.load(std::memory_order_acquire),
			false, _captureArmed.load(), _captureUpScan.load());
		ApplyViewPresentationPolicy();
		BroadcastViewsData();
		REX::INFO("Runtime: replayed {} instantiated view(s) to the replacement browser host; "
				  "overlay left closed", reloaded);
	}

	void Runtime::OnRendererFailure(const WebView2HostWebRenderer::FailureEvent& a_event)
	{
		if (_rendererFailureLatched) {
			return;
		}
		_rendererFailureLatched = true;
		_rendererFailed = true;
		const bool retryableBrowserHostLoss =
			a_event.stage == "host-connection" && _renderer;
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
		m_viewRecovery.ClearAll();

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

	void Runtime::OnSettingChanged(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value)
	{
		// Only the framework's own knobs (mod "osfui"); other mods' settings are
		// theirs to react to. Invoked from main-thread Tick as settings changes
		// dispatch, plus once per value at startup via NotifyAll, so persisted
		// choices apply on boot.
		if (a_modId != "osfui") {
			return;
		}

		// Game-binding conflict warnings (Mod Settings-owned). Lazy build / clear.
		if (a_key == "language" && a_value.is_string()) {
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
		const bool warningChanged = store.SetGameBindingWarningsEnabled(true);
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
			// the compositor's lazy setup so output dimensions can arrive.
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
