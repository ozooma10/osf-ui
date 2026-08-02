#include "runtime/Runtime.h"

#include <cmath>
#include <limits>

#include "RE/C/Calendar.h"

#include "api/BridgeApi.h"
#include "api/PapyrusApi.h"
#include "composite/D3D12Compositor.h"
#include "composite/NullCompositor.h"
#if defined(OSFUI_WITH_WORLD_SURFACES)
#include "composite/ScaleformToTextureProbe.h"
#include "composite/WorldSurface.h"
#endif
#include "composite/UiPassSeam.h"
#include "core/Log.h"
#include "core/StringUtil.h"
#include "core/Version.h"
#include "input/ControlLayer.h"
#include "input/EngineInput.h"
#include "input/FocusMenu.h"
#include "input/FreeCursor.h"
#include "input/HardwareCursor.h"
#include "input/MenuMode.h"
#include "input/OverlayInputHook.h"
#include "input/PauseMenuEntry.h"
#include "input/SimPause.h"
#include "input/XInputPoller.h"
#include "core/Paths.h"
#include "platform/WindowsPlatform.h"
#include "reporting/ReportClient.h"
#include "runtime/Json.h"
#include "runtime/Ids.h"
#include "runtime/PapyrusNames.h"
#include "runtime/VanillaKeys.h"
#include "render/NullWebRenderer.h"
#include "render/WebView2HostWebRenderer.h"

namespace OSFUI
{
	namespace
	{
		constexpr std::string_view kHandoffViewId{ "osfui/handoff" };
		// The Mods surface: pinned warm alongside the handoff (the pause-menu
		// entry and toggle key both land here) and the only source allowed to
		// change player view policy.
		constexpr std::string_view kSettingsViewId{ "osfui/settings" };
		constexpr double           kHandoffDelaySeconds{ 0.15 };
		constexpr double           kReadySignalTimeoutSeconds{ 15.0 };
		constexpr double           kRevealTimeoutSeconds{ 3.0 };
		constexpr KeyCode          kVkF12{ 0x7B };

		// Reads one persisted settings value before the settings module exists.
		// The renderer (and the host process it spawns) initializes earlier than
		// the settings replay, so boot-time consumers peek the values file
		// directly; the OnSettingChanged replay then drives the same Config
		// field. Missing or corrupt file means the schema default.
		bool PeekPersistedBool(const std::filesystem::path& a_valuesFile,
			std::string_view a_key, bool a_default)
		{
			std::error_code ec;
			if (!std::filesystem::exists(a_valuesFile, ec)) {
				return a_default;
			}
			const auto json = Json::ParseFile(a_valuesFile);
			if (!json || !json->is_object()) {
				return a_default;
			}
			return Json::GetBool(*json, a_key, a_default);
		}

	}

	Runtime& Runtime::Get()
	{
		// ExitProcess stops worker threads before DLL static destruction. The
		// runtime owns those workers, so destroying it from process detach can
		// never be made safe; the OS reclaims its in-process resources and the
		// detached WebView2 helper independently watches the game process handle.
		static Runtime* const instance = new Runtime;
		return *instance;
	}

	bool Runtime::Initialize()
	{
		if (_initialized) {
			return true;
		}
		_rendererFailed = false;
		_rendererFailureLatched = false;
		_rendererHostRecovery.Reset();

		if (!Paths::Initialize()) {
			return false;
		}

		_config = Config::Load(Paths::ConfigFile());
		Log::SetDevMode(_config.devMode);

		if (!_config.enabled) {
			REX::INFO("Runtime: disabled via config; nothing further will be initialized");
			return true;
		}
		const auto documents = Platform::GetDocumentsPath();
		const auto starfieldDir = documents.empty() ? std::filesystem::path{} :
			documents / "My Games" / "Starfield";
		_localization.Load(Paths::DataDir() / "l10n",
			LocalizationService::DetectGameLocale(starfieldDir));

		// Label + target view for the injected PauseMenu entry; the main-thread
		// pump gates Reconcile on config.pauseMenuEntry via SetEnabled.
		PauseMenuEntry::Configure(
			_localization.Resolve("osfui", "chrome.pauseMenuEntry", _config.pauseMenuEntryLabel),
			_config.pauseMenuEntryView);
		PauseMenuEntry::SetEnabled(_config.pauseMenuEntry);

		_views.LoadAll(Paths::ViewsDir());
		// Player startup choices live beside the settings values, never in the
		// shipped mod files; loaded before the boot loads below consult it.
		_viewPolicy.Load(Paths::DataDir() / "state" / "view-policy.json");
		std::vector<std::string> discoveredViewIds;
		discoveredViewIds.reserve(_views.All().size());
		for (const auto& manifest : _views.All()) {
			discoveredViewIds.push_back(manifest.id);
		}
		API::BridgeApi::Get().SetViewCatalog(discoveredViewIds);

		_renderer = CreateRenderer();
		const auto* view = _views.Find(_config.view);
		const auto initialWidth = view ? view->width : kDefaultViewWidth;
		const auto initialHeight = view ? view->height : kDefaultViewHeight;
		_viewWidth.store(initialWidth);
		_viewHeight.store(initialHeight);
		_cursorX = initialWidth * 0.5f;
		_cursorY = initialHeight * 0.5f;
		// The WebView2 host receives the report endpoint on its command line at
		// spawn, before the settings module replays persisted values — so the
		// user's bugReporting choice is peeked from the values file here.
		_config.bugReporting = PeekPersistedBool(
			Paths::DataDir() / "settings" / "values" / "osfui.json",
			"bugReporting", _config.bugReporting);
		RendererConfig rendererConfig{
			.width = initialWidth,
			.height = initialHeight,
			.devMode = _config.devMode,
			.reportEndpoint = _config.bugReporting ? kBugReportEndpoint : "",
			.reportPluginRoot = Paths::PluginDir(),
			.dataDir = Paths::DataDir(),
		};
		if (!_renderer->Initialize(rendererConfig)) {
			REX::ERROR("Runtime: renderer '{}' failed to initialize; falling back to null renderer", _renderer->Name());
			_renderer = std::make_unique<NullWebRenderer>();
			_renderer->Initialize(rendererConfig);
		}
		REX::INFO("Runtime: renderer = {}", _renderer->Name());

		// A failed load never fires DOM-ready, so this is the only signal a view
		// didn't come up. Drives crash-recovery.
		_renderer->SetLoadHandler([this](const IWebRenderer::LoadEvent& a_e) {
			OnViewLoad(a_e.viewId, a_e.failed, a_e.url, a_e.description, a_e.errorCode);
		});
		_renderer->SetFailureHandler([this](const IWebRenderer::FailureEvent& a_e) {
			OnRendererFailure(a_e);
		});

		// Degraded-but-alive backend conditions, into System Health (protocol
		// 1.4). Game thread, both edges — see IWebRenderer::HealthEvent.
		_renderer->SetHealthHandler([this](const IWebRenderer::HealthEvent& a_e) {
			_runtimeDiagnostics.OnRendererHealth(a_e);
		});

		// The active page's CSS `cursor` drives the real OS pointer. Unlike the
		// other handlers this fires on the renderer's worker thread (IWebRenderer
		// contract) — SetShape is one atomic store, applied by the WndProc hook on
		// the next mouse message. hardwareCursor is a boot-time config knob, not a
		// setting; the only alternative is an invisible software cursor.
		if (_config.hardwareCursor) {
			_renderer->SetCursorChangeHandler([](CursorShape a_shape) {
				HardwareCursor::SetShape(a_shape);
			});
		}

#if defined(OSFUI_WITH_WORLD_SURFACES)
		// Arm material discovery before the engine device's one hook
		// opportunity. Entries whose view does not exist are dropped HERE,
		// before Configure, so the WorldSurface registry index of every
		// survivor equals its _worldSurfaces position.
		if (!_config.worldSurfaces.empty()) {
			std::vector<WorldSurface::SurfaceDesc> surfaceDescs;
			for (const auto& entry : _config.worldSurfaces) {
				if (!_views.Find(entry.view)) {
					REX::WARN("Runtime: worldSurfaces view '{}' was not found; "
							  "that surface is disabled", entry.view);
					continue;
				}
				const auto index = static_cast<std::uint32_t>(_worldSurfaces.size());
				surfaceDescs.push_back({ .placeholderWidth = entry.placeholderWidth,
					.placeholderHeight = entry.placeholderHeight,
					.label = entry.view });
				_worldSurfaces.push_back({ .renderer = nullptr,
					.viewId = entry.view,
					.surface = index,
					.width = entry.width,
					.height = entry.height });
			}
			const auto accepted = WorldSurface::Configure(surfaceDescs);
			if (accepted != _worldSurfaces.size()) {
				// Configure re-checks basics and never reorders, so a shorter
				// registry can only mean trailing drops; trim to stay 1:1.
				REX::WARN("Runtime: WorldSurface accepted {} of {} surfaces",
					accepted, _worldSurfaces.size());
				_worldSurfaces.resize(accepted);
			}
		}
#endif

		_compositor = CreateCompositor();
		if (!_compositor->Initialize()) {
			REX::WARN("Runtime: compositor '{}' failed to initialize; falling back to null compositor", _compositor->Name());
			_compositor = std::make_unique<NullCompositor>();
			_compositor->Initialize();
		}
		// GPU frame transport (out-of-process WebView2 host): the compositor owns
		// the shared-texture ring handles once handed over. Fires on the game
		// thread (renderer Update()); no-op for CPU-only renderer/compositor pairs.
		_renderer->SetSharedRingHandler([this](const SharedRingDesc& a_desc) {
			if (_compositor) {
				_compositor->SetSharedRing(a_desc);
			}
		});
		// Size the view to the real output so the page renders aspect-correct.
		_compositor->SetOutputResizeCallback([this](std::uint32_t a_w, std::uint32_t a_h) { OnOutputResized(a_w, a_h); });

		// The Scaleform vtables are static, but hook installation is deliberately
		// deferred to SFSE kPostLoad. Luma edits the vanilla Composite body and
		// installs a call-through VMT hook during its Plugin_Load; chaining it is
		// safe only after that work is complete.
#if defined(OSFUI_WITH_WORLD_SURFACES)
		if (_config.devMode) {
			// Investigation-only: characterize Starfield's native
			// Scaleform-to-texture path without putting any additional hook on a
			// normal player's render path. Failure does not affect the overlay.
			ScaleformToTextureProbe::Install();
		}
#endif
		REX::INFO("Runtime: compositor = {}", _compositor->Name());

		_captureInput.store(_config.captureInput);

		// Composition root for feature modules (hosted generically via IUiModule).
		// OnStart() applies persisted state before the first frame.
		BuildModules();
		for (const auto& module : _modules) {
			module->OnStart();
		}

		// One bridge serves every bridge-enabled view. Build it before any view is
		// loaded so a first-open lazy surface gets exactly the same handler wiring
		// as a boot surface. BridgeApi is told it is ready only when LoadSurface
		// actually creates a nativeBridge surface, preserving its readiness contract
		// and pre-ready SendToWeb queue.
		_bridge = std::make_unique<MessageBridge>([this](std::string_view a_viewId, std::string_view a_json) {
			if (_renderer) {
				_renderer->SendMessageToWeb(a_viewId, a_json);
			}
#if defined(OSFUI_WITH_WORLD_SURFACES)
			// View ids are unique across world surfaces (config validation),
			// so at most one instance matches.
			for (auto& worldSurface : _worldSurfaces) {
				if (worldSurface.renderer && !worldSurface.failed &&
					a_viewId == worldSurface.viewId) {
					worldSurface.renderer->SendMessageToWeb(a_viewId, a_json);
				}
			}
#endif
		});
		// The whole host obligation under a page-initiated handshake: answer
		// hellos, in order, with ready then state. Installing it here (rather
		// than open-coding a greeting at each view-creation site) is what makes
		// the ordering guarantee structural.
		_bridge->SetHelloHook([this](std::string_view a_viewId) { OnViewGreeted(a_viewId); });
		_bridge->SetSurfaceFn([this](std::string_view a_viewId, std::string_view a_code,
									  std::string_view a_message, const nlohmann::json& a_detail) {
			OnProtocolMisuse(a_viewId, a_code, a_message, a_detail);
		});
		RegisterPlatformCommands(*_bridge);
		for (const auto& module : _modules) {
			module->RegisterEndpoints(*_bridge);
		}
		_renderer->SetWebMessageHandler([this](std::string_view a_viewId, std::string_view a_json) {
			if (_bridge) {
				_bridge->HandleWebMessage(a_viewId, a_json);
			}
		});


#if defined(OSFUI_WITH_WORLD_SURFACES)
		// Each material-backed surface gets its own host process and capture
		// ring, so opening/closing the fullscreen overlay cannot hide, resize,
		// or replace one, and one crashed surface cannot take down another.
		if (!_worldSurfaces.empty() &&
			(_config.renderer != "webview2" || _config.compositor != "d3d12")) {
			REX::WARN("Runtime: world surfaces require renderer='webview2' and "
					  "compositor='d3d12'; disabled");
			_worldSurfaces.clear();
		}
		for (std::size_t i = 0; i < _worldSurfaces.size(); ++i) {
			auto& instance = _worldSurfaces[i];
			const auto* worldView = _views.Find(instance.viewId);
			if (!worldView) {
				continue;  // vanished since Configure; cannot happen in practice
			}
			auto renderer = CreateRenderer();
			RendererConfig worldConfig{
				.width = instance.width,
				.height = instance.height,
				.devMode = _config.devMode,
				.dataDir = Paths::DataDir(),
				// Own host process, pipe, browser profile, and views mirror per
				// INSTANCE — colliding with the overlay host or a sibling
				// surface host is the proven two-host failure mode.
				.instanceName = std::format("world{}", i + 1),
			};
			if (!renderer->Initialize(worldConfig)) {
				REX::ERROR("Runtime: world-surface renderer '{}' for '{}' failed to "
						   "initialize; that surface stays dark",
					worldConfig.instanceName, instance.viewId);
				continue;  // slot stays null so registry indices keep their pairing
			}
			renderer->SetSharedRingHandler([surface = instance.surface](const SharedRingDesc& a_desc) {
				WorldSurface::SetSharedRing(surface, a_desc);
			});
			renderer->SetWebMessageHandler([this](std::string_view a_viewId,
				std::string_view a_json) {
				if (_bridge) {
					_bridge->HandleWebMessage(a_viewId, a_json);
				}
			});
			renderer->SetLoadHandler([](const IWebRenderer::LoadEvent& a_e) {
				if (a_e.failed) {
					REX::ERROR("Runtime: world surface '{}' failed to load: {} ({})",
						a_e.viewId, a_e.description, a_e.errorCode);
				}
			});
			// Terminal host failure: log, flag, and let Tick shut the instance
			// down NEXT tick — the callback fires from inside renderer Update().
			// No session recovery and no menu/input side effects to release:
			// unlike the overlay path (OnRendererFailure), a dead world surface
			// just leaves its mesh on the placeholder pattern.
			renderer->SetFailureHandler([this, i](const IWebRenderer::FailureEvent& a_e) {
				REX::ERROR("Runtime: world surface host '{}' failed at '{}' "
						   "(0x{:08X}): {} - disabling that surface for this session",
					i < _worldSurfaces.size() ? _worldSurfaces[i].viewId : "?",
					a_e.stage, a_e.errorCode, a_e.description);
				if (i < _worldSurfaces.size()) {
					_worldSurfaces[i].failed = true;
				}
			});
			renderer->SetHealthHandler([this, i](const IWebRenderer::HealthEvent& a_e) {
				_runtimeDiagnostics.OnWorldSurfaceHealth(i, a_e);
			});
			auto manifest = *worldView;
			manifest.transparent = false;
			manifest.width = instance.width;
			manifest.height = instance.height;
			renderer->LoadView(manifest);
			renderer->SetActiveView(manifest.id);
			renderer->SetViewHidden(manifest.id, false);
			instance.renderer = std::move(renderer);
			// The C ABI's SendToWeb holdback releases a target only once its page
			// exists (SetSurfaceLoaded). World surfaces never pass through
			// LoadSurface, so mark them here — the MessageBridge send callback
			// above already fans deliveries out to this instance's renderer.
			API::BridgeApi::Get().SetSurfaceLoaded(instance.viewId, true);
			REX::INFO("Runtime: world surface '{}' ('{}') started at {}x{} — "
					  "dedicated browser host process",
				instance.viewId, worldConfig.instanceName,
				instance.width, instance.height);
		}
#endif

		// The pinned core set: platform surfaces that must never pay a cold
		// first paint. Established before the first LoadSurface so lifecycle
		// policy records the correct never-destroy bit. Deliberately not
		// configurable — everything else is discovered and loads on first open.
		_warmViews.clear();
		for (const auto id : { kHandoffViewId, kSettingsViewId }) {
			if (_views.Find(id)) {
				_warmViews.emplace(id);
			}
		}

		std::size_t loaded = 0;
		const auto loadWarm = [this, &loaded](std::string_view a_id,
			std::string_view a_reason) {
			const auto* manifest = _views.Find(a_id);
			const bool wasRegistered = _menus.IsRegistered(a_id);
			if (!manifest || !LoadSurface(*manifest, a_reason)) {
				return;
			}
			if (!wasRegistered) {
				++loaded;
			}
			// Prime one hidden paint so latency-sensitive surfaces do not pay both
			// controller startup and page paint on their first reveal.
			_renderer->PrewarmView(a_id);
		};
		loadWarm(kHandoffViewId, "as the warm first-load handoff");
		loadWarm(kSettingsViewId, "as the pinned Mods surface");

		// HUD automatic start: the manifest's openOnStart is only the author
		// default — the player's per-HUD choice (ViewPolicyStore, set from the
		// Mods surface) wins. Menus never auto-start from discovery; a plugin's
		// explicit RegisterView still honors openOnStart (ABI 1.5), and every
		// other view loads on its first open.
		for (const auto& manifest : _views.All()) {
			if (manifest.kind != SurfaceKind::Hud || _warmViews.contains(manifest.id)) {
				continue;
			}
			if (!HudAutoStartEligible(manifest)) {
				if (_viewPolicy.HasHudOverride(manifest.id)) {
					REX::DEBUG("Runtime: HUD '{}' has an auto-start override but is not "
							   "eligible (hub:false or debugOnly without Debug mode); ignored",
						manifest.id);
				}
				continue;
			}
			if (!_viewPolicy.HudAutoStart(manifest.id, manifest.openOnStart)) {
				continue;
			}
			if (LoadSurface(manifest, "for HUD auto-start")) {
				++loaded;
				if (_menus.IsRegistered(manifest.id)) {
					_menus.Open(manifest.id);
				}
			}
		}
		REX::INFO("Runtime: loaded {} warm/auto-start view(s); default menu = '{}'",
			loaded, _config.view);
		if (!_views.Find(_config.view)) {
			REX::WARN("Runtime: default view '{}' was not discovered; the toggle key will have nothing to open",
				_config.view);
		}

		// Key events reach the router from the WndProc subclass (OverlayInputHook
		// → OnHostKey), installed when config inputSource="ui" (core/Plugin.cpp,
		// kPostPostDataLoad).
		const auto toggleKey = ResolveKeyName(_config.toggleKey);
		_toggleKey.store(toggleKey, std::memory_order_release);
		if (toggleKey != kInvalidKeyCode) {
			REX::INFO("Runtime: toggleKey '{}' resolved to VK code {:#x}", _config.toggleKey, toggleKey);
		}
		EngineInput::SetEnabled(_config.engineInput);
		if (_config.engineInput) {
			REX::INFO("Runtime: engineInput enabled — engine per-menu input (gamepad) routed into the focused view; keyboard/mouse stay on the WndProc path");
		}

		// Toggle key opens/closes the default menu; Esc (while captured) is the back
		// action — close the top menu, or delegate to a back-owning view
		// (osfui.handleBack). Separate so a live rebind can re-apply it.
		ApplyToggleKey();
		_renderer->SetNativeAcceleratorHandler(
			[this](std::uint32_t a_vkCode, bool a_down) {
				return OnNativeAcceleratorKey(a_vkCode, a_down);
			});

		_input.SetWebRouting(
			[this] { return IsInputCaptured(); },
			[this](KeyCode a_key, bool a_down) {
				if (_renderer) {
					_renderer->InjectKeyEvent(a_key, a_down);
				}
			});
		REX::INFO("Runtime: input capture {} (config captureInput)", _config.captureInput ? "enabled" : "disabled");

        if (_config.devMode) {
            _devViewReload = std::make_unique<DevViewReloadWorker>(
                Paths::ViewsDir(), [this](const DevViewReloadWorker::Target& a_target) {
                    bool refreshed = true;
                    if (a_target.overlay) {
                        refreshed = _renderer && _renderer->RefreshViewFiles(a_target.id);
                    }
#if defined(OSFUI_WITH_WORLD_SURFACES)
                    if (refreshed && a_target.world) {
                        bool anyWorld = false;
                        for (auto& worldSurface : _worldSurfaces) {
                            if (worldSurface.renderer && !worldSurface.failed &&
                                worldSurface.viewId == a_target.id) {
                                anyWorld = true;
                                refreshed = refreshed &&
                                    worldSurface.renderer->RefreshViewFiles(a_target.id);
                            }
                        }
                        refreshed = refreshed && anyWorld;
                    }
#endif
                    return refreshed;
                });
        }

		_initialized = true;
		// Push the initial policy derived from whatever is open (incl. nothing).
		ApplyMenuPolicy();
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

	void Runtime::Tick(double a_deltaSeconds)
	{
		if (!_initialized) {
			return;
		}
		_uptime += a_deltaSeconds;
		// A failure callback fires near the end of the prior Tick. Restart only now,
		// after IWebRenderer::Update has returned and its notification drain is idle.
		DriveRendererHostRecovery();
		DrainBugReportResult();
		// The pause-menu entry (PauseMenuEntry::Reconcile) is NOT driven from
		// here: although Tick runs on the game main thread, arbitrary Scaleform
		// access must also avoid re-entering the AS3 VM. MainThreadMenuPump drives
		// it post-UI_AdvanceActiveMenus, when every admitted movie has finished
		// its frame; a click's EnqueueOpenView lands below on the next tick.
		// Validate plugin-supplied view registrations (ABI 1.5) before the menu-
		// request snapshot below. Ordinary views remain lazy; openOnStart views are
		// created here.
		DrainViewRegistrations();
		// Snapshot queued menu requests (toggle/Esc/transition + plugin
		// RequestMenu) now, but apply them after the bridge pump below — the ABI
		// 1.3 ordering guarantee: a consumer that called SendToWeb(v, ...) then
		// RequestMenu(v, true) has its send in _pendingSends before the request
		// entered this snapshot, so the pump flushes the message into v's queue
		// before the open unhides v (message before first visible paint).
		const auto menuWork = TakeMenuRequests();
		// Load discovered targets while they are still hidden, before queued sends
		// are pumped. ApplyMenuRequests performs only the visibility transition.
		PrepareMenuRequests(menuWork);
		// Deliver a captured rebind key back to the settings view (main thread).
		DrainKeyCapture();
		// Deliver queued hotkey fires (window thread -> main, mcm-design.md §9)
		// before the bridge pump below, so the C ABI callbacks they queue are
		// invoked this same tick.
		DrainHotkeys();
		// Apply queued runtime schema (un)registrations to the store first, so
		// their value replay is already queued when the pump below drains
		// SubscribeSettings callbacks — registration lands in one tick.
		DrainSchemaOps();
		// Papyrus Set*/Reset ops (mcm-design.md §8.4) go through the same validated
		// store path as every other writer. After DrainSchemaOps so a set against a
		// just-registered schema resolves this tick.
		if (_settings) {
			API::Papyrus::DrainSettingsOps(_settings->Store());
		}
		// Papyrus state and events reach the publishing mod's live views before
		// PumpMainThread/Update flush the per-view outbound queues, so both land
		// in this tick's frame. No subscriber set: the target list is derived
		// fresh from the live surfaces each time, so there is nothing to prune
		// or go stale.
		if (_bridge) {
			// A game load reset the VM: drop retained PAPYRUS state, whose values
			// can hold session-scoped form identities. Native plugin state is
			// left alone — a plugin's HUD config has no such lifetime, and
			// wiping it on every load would be the bug.
			if (API::Papyrus::TakeSessionReset()) {
				_viewState.ClearSessionScoped();
			}
			// SetView* is RETAINED: it goes into the shared store first, so a
			// document that greets the bridge later is replayed the same value.
			// This is why a Papyrus-backed HUD survives F5 with no re-push
			// handshake in the script.
			API::Papyrus::DrainViewState([this](const API::Papyrus::ViewState& a_state) {
				_viewState.Set(a_state.mod, a_state.key, a_state.value, /*sessionScoped*/ true);
				PublishModState(a_state.mod, a_state.key, a_state.value);
			});
			// The native ABI's half of the same grid (SetViewState). Same store,
			// same replay — a plugin sets a value once and every fresh document
			// of its mod is handed it, exactly like Papyrus state. NOT
			// session-scoped: a plugin's state holds no form identities.
			for (auto& op : API::BridgeApi::Get().TakeViewStateOps()) {
				_viewState.Set(op.mod, op.key, op.value, /*sessionScoped*/ false);
				PublishModState(op.mod, op.key, op.value);
			}
			// SendViewEvent is a one-shot happening: never retained, never
			// replayed. Encoding one as state would re-fire its effect on every
			// reload, which is exactly the bug the split exists to prevent.
			API::Papyrus::DrainViewEvents([this](const API::Papyrus::ViewEvent& a_event) {
				const auto targets = LiveViewsOfMod(a_event.mod);
				if (targets.empty()) {
					REX::DEBUG("Runtime: SendViewEvent {}.{} had no live '{}/...' view to deliver to",
						a_event.mod, a_event.name, a_event.mod);
					return;
				}
				_bridge->Emit(targets, std::format("{}.{}", a_event.mod, a_event.name),
					nlohmann::json{ { "args", a_event.args } });
			});
			API::Papyrus::DrainViewReplies([this](const API::Papyrus::ViewReply& reply) {
				if (reply.rejected) {
					_bridge->RejectTo(reply.requestId, reply.code, reply.message);
				} else {
					_bridge->RespondTo(reply.requestId, nlohmann::json{ { "value", reply.value } });
				}
			});
		}
		// Expire deferred requests past the host deadline with `no-response`,
		// before the pump below, so a backend that stopped answering frees the
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
		ApplyMenuRequests(menuWork);
		// Land coalesced settings value writes once their write-behind window
		// elapses (mcm-design.md §8.1) — a slider drag costs one disk write per
		// ~500ms, not one per step.
		if (_settings) {
			_settings->Store().PumpPersistence(_uptime);
			// Schema hot-reload (mcm-design.md §12.1, devMode): edited
			// settings/*.json files reload live, values preserved; the
			// registry re-broadcast repaints any open settings view.
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
		// state (not visibility): a live HUD must not disable controls.
		if (_config.focusMenu) {
			ReconcileFocusMenu();
		}
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
		FreeCursor::Apply(_menus.DesiredCapture());
		if (_config.engineInput) {
			DrainEngineInput(a_deltaSeconds);
		}
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
			// Out-of-process backends mirror the accelerator state so their host
			// process can decide `handled` synchronously; pushed every tick,
			// backends diff and forward only changes (default no-op).
			_renderer->SetAcceleratorKeys(_toggleKey.load(std::memory_order_acquire),
				IsInputCaptured(), _captureArmed.load(), _captureUpVk.load());
			_renderer->Update(a_deltaSeconds);
			DrivePendingOpen();
			SubmitFrameIfVisible();
			UpdateRenderDiagnostics();
		}
#if defined(OSFUI_WITH_WORLD_SURFACES)
		for (auto& worldSurface : _worldSurfaces) {
			if (!worldSurface.renderer) {
				continue;
			}
			if (worldSurface.failed) {
				// Deferred from the failure callback, which fires inside the
				// renderer's own Update(). The mesh keeps the placeholder.
				// Release the holdback flag set at startup unless the same id is
				// also a live overlay surface (LoadSurface owns that bit); later
				// sends queue bounded instead of feeding a dead host.
				if (!_menus.IsRegistered(worldSurface.viewId)) {
					API::BridgeApi::Get().SetSurfaceLoaded(worldSurface.viewId, false);
				}
				worldSurface.renderer.reset();
				continue;
			}
			worldSurface.renderer->Update(a_deltaSeconds);
			if (const auto worldFrame = worldSurface.renderer->Render()) {
				WorldSurface::Submit(worldSurface.surface, *worldFrame);
			}
		}
		if (!_worldSurfaces.empty()) {
			// Unconditional: Render() only yields on repaint, and the engine may
			// restore any placeholder descriptor at any time.
			WorldSurface::Refresh();
		}
#endif
		// After Update(), so health edges raised by either renderer this tick are
		// in the registry before the snapshot goes out.
		_runtimeDiagnostics.Pump();
	}

	void Runtime::EnqueueMenuRequest(MenuReq a_req)
	{
		// Callable from any thread (WndProc toggle/Esc, MenuEventSink transition).
		// Leaf lock: it only guards the queue; the request is acted on in Tick.
		std::lock_guard lock(_reqMutex);
		_reqs.push_back(a_req);
	}

	void Runtime::EnqueueOpenView(std::string a_viewId)
	{
		// Callable from any thread (PauseMenuEntry click). Same leaf-lock
		// discipline as EnqueueMenuRequest.
		std::lock_guard lock(_reqMutex);
		_openViewReqs.push_back(std::move(a_viewId));
	}

	bool Runtime::LoadSurface(const ViewManifest& a_manifest, std::string_view a_reason)
	{
		const auto& id = a_manifest.id;
		if (_menus.IsRegistered(id)) {
			return true;
		}
		if (!_renderer) {
			return false;
		}

		// Install diagnostics before navigation so even the earliest page console
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
		_readyViews.erase(id);
		_renderer->LoadView(a_manifest);
		_renderer->SetRenderStats(id, _renderStatsEnabled);
		// A fresh view starts at manifest dimensions; restore the current
		// output-matched size. Before first present these are the initialized
		// logical dimensions and the normal output-resize path supersedes them.
		if (const auto w = _viewWidth.load(), h = _viewHeight.load(); w && h) {
			_renderer->Resize(w, h);
		}
		_menus.Register({ id, a_manifest.kind, a_manifest.capturesInput,
			a_manifest.pausesGame, a_manifest.order });
		_viewLifecycle.NoteLoaded(id, _warmViews.contains(id), _uptime);
		API::BridgeApi::Get().SetSurfaceLoaded(id, true);

		REX::INFO("Runtime: surface '{}' loaded {} ({}, capturesInput={}, pausesGame={})",
			id, a_reason, a_manifest.kind == SurfaceKind::Hud ? "hud" : "menu",
			a_manifest.capturesInput, a_manifest.pausesGame);
		if (a_manifest.permissions.nativeBridge && _bridge) {
			// This may be the first bridge-enabled surface. Publish the bridge before
			// this tick's PumpMainThread so queued sends reach the newly created
			// renderer view.
			API::BridgeApi::Get().OnBridgeReady(_bridge.get());
			// Arm a closed event gate. The greeting is the PAGE's move now, so
			// nothing is pushed here: events raised before the document says hello
			// queue behind the gate, and every current state value is replayed when
			// it does. That is the whole boot path, identically for a first open, an
			// F5, a dev hot-reload and a crash-recovery reload.
			_bridge->OnViewCreated(id);
		}
		return true;
	}

	Runtime::PendingMenuWork Runtime::TakeMenuRequests()
	{
		// Snapshot under the lock, then act unlocked (in ApplyMenuRequests): the
		// actions call into the renderer/compositor and must never run while
		// holding _reqMutex.
		PendingMenuWork work;
		{
			std::lock_guard lock(_reqMutex);
			work.local.swap(_reqs);
			work.openViews.swap(_openViewReqs);
		}
		// Sibling-plugin opens/closes by id; same policy path as the toggle key.
		work.plugin = API::BridgeApi::Get().TakeMenuRequests();
		return work;
	}

	void Runtime::PrepareMenuRequests(const PendingMenuWork& a_work)
	{
		const auto prepare = [this](std::string_view a_id, std::string_view a_reason) {
			if (_menus.IsRegistered(a_id)) {
				return;
			}
			if (const auto* manifest = _views.Find(a_id)) {
				LoadSurface(*manifest, a_reason);
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
		if (!_pendingSurfaceOpen && !_menus.ActiveMenu() &&
			std::ranges::find(a_work.local, MenuReq::ToggleDefault) != a_work.local.end()) {
			prepare(_config.view, "for the default-menu toggle");
		}
	}

	void Runtime::ApplyMenuRequests(const PendingMenuWork& a_work)
	{
		const auto& reqs = a_work.local;
		const auto& pluginReqs = a_work.plugin;
		if (reqs.empty() && pluginReqs.empty() && a_work.openViews.empty()) {
			return;
		}
		for (const auto req : reqs) {
			switch (req) {
			case MenuReq::ToggleDefault:
				if (_pendingSurfaceOpen) {
					CancelPendingOpen();
				} else if (_menus.ActiveMenu()) {
					_menus.CloseTop();
				} else {
					BeginSurfaceOpen(_config.view);
				}
				break;
			case MenuReq::Back: {
				// Esc / pad-B. A back-owning active view (osfui.handleBack) gets
				// the action delegated as a synthetic Escape tap and decides for
				// itself — navigate elsewhere, peel an inner panel, or send
				// `close`. Everyone else closes the top menu (single-menu policy:
				// that hides the overlay). The toggle key never delegates, so a
				// broken page cannot strand the user.
				const auto active = _menus.ActiveMenu();
				if (_pendingSurfaceOpen && (!active || *active == kHandoffViewId)) {
					CancelPendingOpen();
				} else if (active && _backOwnerViews.contains(*active) && _renderer) {
					constexpr std::uint32_t kVkEscape = 0x1B;
					_renderer->InjectKeyEvent(kVkEscape, true);
					_renderer->InjectKeyEvent(kVkEscape, false);
				} else {
					_menus.CloseTop();
				}
				break;
			}
			case MenuReq::CloseAll:
				CancelPendingOpen();
				_menus.CloseAll();
				break;
			}
		}
		for (const auto& id : a_work.openViews) {
			if (!_menus.IsRegistered(id)) {
				REX::WARN("Runtime: EnqueueOpenView('{}') ignored — no discovered surface could be loaded", id);
			} else {
				BeginSurfaceOpen(id);
			}
		}
		for (const auto& r : pluginReqs) {
			if (r.open) {
				if (!_menus.IsRegistered(r.view)) {
					REX::WARN("Runtime: plugin RequestMenu('{}', open) could not load the discovered surface", r.view);
				} else {
					BeginSurfaceOpen(r.view);
				}
			} else {
				if (_pendingSurfaceOpen &&
					(_pendingSurfaceOpen->target == r.view || r.view == kHandoffViewId)) {
					CancelPendingOpen();
				}
				_menus.Close(r.view);
			}
		}
		ApplyMenuPolicy();
	}

	bool Runtime::BeginSurfaceOpen(std::string_view a_id)
	{
		if (!_overlayDrawAvailable.load(std::memory_order_acquire)) {
			REX::WARN("Runtime: cannot open '{}' — the Scaleform UI draw path is unavailable",
				a_id);
			return false;
		}
		if (_rendererFailed) {
			if (_rendererHostRecovery.RequestManualRetry(_uptime)) {
				REX::INFO("Runtime: open of '{}' requested a fresh WebView2 helper recovery cycle; "
					"the overlay remains closed until the replacement is ready", a_id);
			} else if (_rendererHostRecovery.PhaseValue() ==
				RendererHostRecovery::Phase::Waiting ||
				_rendererHostRecovery.PhaseValue() ==
				RendererHostRecovery::Phase::AwaitingResponse) {
				REX::WARN("Runtime: cannot open '{}' yet - the WebView2 helper is recovering", a_id);
			} else {
				REX::WARN("Runtime: cannot open '{}' - the Web renderer needs a game restart or "
					"the repair described in the log", a_id);
			}
			return false;
		}
		if (!_menus.IsRegistered(a_id)) {
			return false;
		}
		const auto* manifest = _views.Find(a_id);
		if (!manifest || manifest->kind == SurfaceKind::Hud ||
			a_id == kHandoffViewId || !_menus.IsRegistered(kHandoffViewId)) {
			CancelPendingOpen();
			return _menus.Open(a_id);
		}

		const auto loadState = GetViewLoadState(a_id);
		if (IsViewReady(a_id, *manifest, loadState)) {
			CancelPendingOpen();
			return _menus.Open(a_id);
		}
		if (_pendingSurfaceOpen && _pendingSurfaceOpen->target == a_id) {
			return false;
		}

		CancelPendingOpen();
		PendingSurfaceOpen pending;
		pending.target = std::string(a_id);
		pending.startedAt = _uptime;
		if (loadState == ViewLoadState::Finished) {
			pending.loadedAt = _uptime;
		}
		_pendingSurfaceOpen = std::move(pending);
		REX::DEBUG("Runtime: holding first open of '{}' until the view is ready", a_id);
		return true;
	}

	bool Runtime::CancelPendingOpen()
	{
		if (!_pendingSurfaceOpen) {
			return false;
		}
		const auto target = _pendingSurfaceOpen->target;
		const bool changed = _menus.Close(kHandoffViewId);
		_pendingSurfaceOpen.reset();
		REX::DEBUG("Runtime: cancelled pending open of '{}'", target);
		return changed;
	}

	void Runtime::ShowHandoff(std::string_view a_phase, bool a_retry)
	{
		if (!_pendingSurfaceOpen || !_bridge) {
			return;
		}
		auto& pending = *_pendingSurfaceOpen;
		const auto* target = _views.Find(pending.target);
		if (!target || !_menus.IsRegistered(kHandoffViewId)) {
			return;
		}
		const bool stateChanged = !pending.handoffVisible || pending.phase != a_phase ||
			pending.error != a_retry;
		if (!stateChanged) {
			return;
		}

		// The warm surface borrows the target menu's policy, so loading feels
		// like entering that same terminal instead of opening global UI chrome.
		_menus.Register({ std::string(kHandoffViewId), SurfaceKind::Menu,
			target->capturesInput, target->pausesGame, target->order });
		const auto title = _localization.Resolve(target->mod,
			"views." + std::string(Ids::ViewNameOf(target->id)) + ".title", target->title);
		// STATE, not an event: this is latest-wins data the handoff surface
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
		_menus.Open(kHandoffViewId);
		pending.handoffVisible = true;
		pending.phase = std::string(a_phase);
		pending.error = a_retry;
		ApplyMenuPolicy();
	}

	void Runtime::FinishPendingOpen()
	{
		if (!_pendingSurfaceOpen) {
			return;
		}
		const auto target = _pendingSurfaceOpen->target;
		_menus.Close(kHandoffViewId);
		_menus.Open(target);
		_pendingSurfaceOpen.reset();
		REX::DEBUG("Runtime: first-load handoff completed for '{}'", target);
		ApplyMenuPolicy();
	}

	void Runtime::DrivePendingOpen()
	{
		if (!_pendingSurfaceOpen) {
			return;
		}
		auto& pending = *_pendingSurfaceOpen;
		const auto* manifest = _views.Find(pending.target);
		if (!manifest) {
			ShowHandoff("error", true);
			return;
		}
		// An unregistered target is exactly the state the retry exists to
		// recover from (OnViewLoad's exhaustion path destroys the view and
		// unregisters it), so only park on the error screen when no retry is
		// pending.
		if (!_menus.IsRegistered(pending.target) && !pending.retryRequested) {
			ShowHandoff("error", true);
			return;
		}
		if (pending.retryRequested) {
			pending.retryRequested = false;
			if (!_renderer) {
				return;
			}
			if (!_menus.IsRegistered(pending.target)) {
				if (!LoadSurface(*manifest, "for first-load handoff retry")) {
					ShowHandoff("error", true);
					return;
				}
			} else {
				_recovery.erase(pending.target);
				// ReloadViewInPlace sends runtime.ready itself now, for every
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
		if (IsViewReady(pending.target, *manifest, state)) {
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
		if (_pendingSurfaceOpen && _pendingSurfaceOpen->error) {
			_pendingSurfaceOpen->retryRequested = true;
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
				REX::WARN("Runtime: UnregisterSettingsSchema('{}') ignored — not a runtime-registered schema", op.modId);
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
			// Idempotent: reloading a live surface (config-listed or a repeat
			// call) would blow away its page state.
			if (_menus.IsRegistered(id)) {
				REX::DEBUG("Runtime: plugin RegisterView('{}') — already a registered surface, left untouched", id);
				continue;
			}
			const auto* m = _views.Find(id);
			if (!m) {
				REX::WARN("Runtime: plugin RegisterView('{}') ignored — no views/{}/manifest.json was discovered at boot (ids are qualified '<author>.<modname>/<view>'; is the view folder installed?)", id, id);
				continue;
			}
			if (m->openOnStart) {
				if (!LoadSurface(*m, "via plugin RegisterView openOnStart")) {
					continue;
				}
				_menus.Open(id);
				catalogChanged = true;
			} else {
				// Discovery already made this id catalogued and RequestMenu-openable.
				// RegisterView now validates intent while deferring page creation.
				REX::DEBUG("Runtime: plugin RegisterView('{}') accepted; creation deferred until first open", id);
			}
		}
		if (catalogChanged) {
			ApplyMenuPolicy();     // openOnStart / z-band changes take effect now
			BroadcastViewsData();  // the Mods surface picks the new view up live
		}
	}

	void Runtime::ApplyMenuPolicy()
	{
		if (!_renderer) {
			return;
		}
		// All menu-opening paths converge here, including legacy RegisterView and
		// a page asking to show itself. Keep HUD state, but never let an interactive
		// menu claim focus/input when the compositor cannot put it on screen.
		if (!_overlayDrawAvailable.load(std::memory_order_acquire) &&
			_menus.ActiveMenu()) {
			REX::WARN("Runtime: closing a requested menu because the Scaleform UI draw path is unavailable");
			_menus.CloseTop();
		}
		// Per-surface hidden + composite z, derived from the band order: HUDs
		// beneath menus; HUDs by `order`, menus by open-stack position.
		for (const auto& layer : _menus.DesiredLayers()) {
			_renderer->SetViewHidden(layer.id, layer.hidden);
			_viewLifecycle.NoteVisibility(layer.id, !layer.hidden, _uptime);
			_viewLifecycle.NoteOpenState(layer.id, _menus.IsOpen(layer.id), _uptime);
			_renderer->SetViewOrder(layer.id, layer.z);
		}
		// Focus follows the top menu; HUD-only => no active view to set.
		const auto active = _menus.ActiveMenu();
		if (active) {
			_renderer->SetActiveView(*active);
		}
		// Capture requires both the global config gate and the top menu's policy
		// (false for HUD-only => the game keeps input).
		const bool desiredCapture = _config.captureInput && _menus.DesiredCapture();
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
		const bool visible = _menus.DesiredVisible();
		const bool wasVisible = _visible.exchange(visible);
		// Interactive menus use real browser focus for the full session so Windows
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
				_revealPending = true;
				_revealFrameReady = false;
				_revealHeldSeconds = 0.0;
				_revealLastPolledAt = {};
			} else {
				if (!visible) {
					_revealPending = false;  // closed while a reveal was still pending
					_revealFrameReady = false;
					_revealHeldSeconds = 0.0;
					_revealLastPolledAt = {};
				}
				if (!_revealPending) {
					_compositor->SetVisible(visible);
				}
			}
		}

		// Open->closed edge: flush the settings write-behind instead of waiting
		// out the window (mcm-design.md §8.1; the shutdown flush is
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
		// ui.visibility keys off the shown view (the focused menu of a visible
		// overlay) changing, not off the overlay's open/close edge: a view switch
		// while the overlay stays up (hub -> panel) is a real show for the new view
		// and a real hide for the old one. Consumers arm whole sessions off this
		// signal, so an edge-only send left hub-opened views permanently "closed".
		// The hide can't render a fade-out (the compositor already hid this frame
		// on the overlay-close path), but the view's JS keeps running while hidden.
		// By overlay close ActiveMenu() is already empty, hence the tracked name.
		if (_bridge) {
			const std::string shown = (visible && active) ? *active : std::string();
			if (shown != _lastShownView) {
				// reason lets views scope per-overlay-visit state to real overlay
				// edges while still seeing focus handoffs: "overlay" = the overlay
				// opened/closed this tick, "focus" = only the focused menu changed.
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
		const auto active = _menus.ActiveMenu();
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
		// log. Validate against the live registry — every sibling surface
		// command does — not the boot list, which a drop-in view opened via
		// menu.open is never on.
		if (!_menus.IsRegistered(a_id)) {
			REX::WARN("Runtime: setViewHidden ignored — '{}' is not a loaded view", a_id);
			return false;
		}
		if (_renderer) {
			_renderer->SetViewHidden(a_id, a_hidden);
		}
		// Keep lifecycle policy in step with this out-of-band visibility edge,
		// exactly like ApplyMenuPolicy does for policy-driven layers. Without it
		// a view revealed here still ages as hidden and idle reclaim would
		// destroy it while it is on screen (and the suspend handshake desyncs:
		// the host refuses a suspend for a visible page the game thinks hidden).
		_viewLifecycle.NoteVisibility(a_id, !a_hidden, _uptime);
		REX::DEBUG("Runtime: view '{}' hidden -> {}", a_id, a_hidden);
		return true;
	}

	void Runtime::OnViewLoad(std::string_view a_viewId, bool a_failed, std::string_view a_url,
		std::string_view a_description, int a_errorCode)
	{
		const std::string id(a_viewId);
		if (_rendererFailed && _rendererHostRecovery.CanAcceptResponse()) {
			const auto attempts = _rendererHostRecovery.Attempts();
			_rendererHostRecovery.Reset();
			_rendererFailed = false;
			_rendererFailureLatched = false;
			REX::INFO("Runtime: replacement WebView2 helper responded on attempt {}; "
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
			_runtimeDiagnostics.ReportViewLoad(a_viewId, false, {}, 0, 0);
			BroadcastViewsData();  // loadState loading -> loaded
			return;
		}

		REX::ERROR("Runtime: view '{}' FAILED to load ({}): {} [{}]",
			a_viewId, a_url, a_description, a_errorCode);

		// Crash-recovery: schedule a bounded reload with backoff. attempts counts
		// reloads already fired; an exhausted budget means the content is broken,
		// so tear the view down and unregister its surface — otherwise the toggle
		// key / menu.open can re-open an invisible, input-capturing shell.
		constexpr std::uint32_t kMaxAttempts = 3;
		constexpr double        kBackoffSec[kMaxAttempts] = { 2.0, 5.0, 15.0 };
		auto& rec = _recovery[id];
		if (rec.attempts >= kMaxAttempts) {
			REX::ERROR("Runtime: view '{}' still failing after {} reload attempts; giving up — "
					   "destroying the view and removing its surface (fix the view's files and relaunch)",
				a_viewId, rec.attempts);
			// The retry budget is spent: this is the error a player has to act on.
			_runtimeDiagnostics.ReportViewLoad(a_viewId, true, a_description, a_errorCode, 0);
			TearDownSurface(id, SurfaceTeardownReason::LoadExhausted);
			return;
		}
		rec.pending = true;
		rec.retryAt = _uptime + kBackoffSec[rec.attempts];
		REX::WARN("Runtime: view '{}' reload attempt {}/{} scheduled in {:.0f}s",
			a_viewId, rec.attempts + 1, kMaxAttempts, kBackoffSec[rec.attempts]);
		_runtimeDiagnostics.ReportViewLoad(a_viewId, true, a_description, a_errorCode,
			kMaxAttempts - rec.attempts);
		BroadcastViewsData();  // loadState -> failed
	}

	bool Runtime::IsViewReady(std::string_view a_id, const ViewManifest& a_manifest, ViewLoadState a_state) const
	{
		return a_manifest.readySignal ? _readyViews.contains(std::string(a_id)) :
										a_state == ViewLoadState::Finished;
	}

	void Runtime::ReloadViewInPlace(const std::string& a_id, const ViewManifest& a_manifest)
	{
		_viewLoadState[a_id] = ViewLoadState::Loading;
		_readyViews.erase(a_id);
		_viewLifecycle.NoteActivity(a_id, _uptime);
		_renderer->LoadView(a_manifest);
		if (a_manifest.permissions.nativeBridge && _bridge) {
			// Re-arm the gate: the replacement document greets the bridge itself and
			// is replayed then. This is where 1.x had to race a host-initiated
			// greeting against the navigate (and lean on the host's domSeen reset to
			// keep it off the outgoing page) — a page-initiated handshake cannot
			// reach the wrong document by construction.
			_bridge->OnViewCreated(a_id);
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
				continue;  // shouldn't happen: only loaded (known) views get load events
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
			return !_menus.IsRegistered(a_id) ||
			       GetViewLoadState(a_id) == ViewLoadState::Loading ||
			       _recovery.contains(a_id) ||
			       (_pendingSurfaceOpen && _pendingSurfaceOpen->target == a_id);
		};
		for (const auto& id : actions.suspend) {
			if (unavailable(id) ||
				(id == kHandoffViewId && _pendingSurfaceOpen.has_value())) {
				continue;
			}
			_renderer->SuspendView(id);
			_viewLifecycle.NoteSuspendRequested(id);
		}
		for (const auto& id : actions.destroy) {
			if (unavailable(id) || _menus.IsOpen(id)) {
				continue;
			}
			TearDownSurface(id, SurfaceTeardownReason::IdleReclaim);
		}
	}

	void Runtime::TearDownSurface(const std::string& a_id, SurfaceTeardownReason a_reason)
	{
		_recovery.erase(a_id);
		_readyViews.erase(a_id);
		if (a_reason == SurfaceTeardownReason::IdleReclaim) {
			_viewLoadState.erase(a_id);
		}
		if (_renderer) {
			_renderer->DestroyView(a_id);
		}
		if (_menus.Unregister(a_id)) {
			ApplyMenuPolicy();  // crash teardown may need to release input/pause now
		}
		API::BridgeApi::Get().SetSurfaceLoaded(a_id, false);
		bool bridgeSurfaceRemains = false;
		for (const auto& manifest : _views.All()) {
			if (manifest.permissions.nativeBridge && _menus.IsRegistered(manifest.id)) {
				bridgeSurfaceRemains = true;
				break;
			}
		}
		if (!bridgeSurfaceRemains) {
			API::BridgeApi::Get().OnBridgeReady(nullptr);
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
		if (a_reason == SurfaceTeardownReason::IdleReclaim) {
			REX::INFO("Runtime: reclaimed idle view '{}' after {:.0f} minutes hidden; it will reload on next open",
				a_id, ViewLifecycle::kDestroyAfterHiddenSeconds / 60.0);
		}
		BroadcastViewsData();
	}

	void Runtime::DriveDevTools()
	{
		if (!_devToolsRequested.exchange(false) || !_renderer || !_config.devMode) {
			return;
		}
		const auto active = _menus.ActiveMenu();
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
            const bool overlay = _menus.IsRegistered(manifest.id);
#if defined(OSFUI_WITH_WORLD_SURFACES)
            const bool world = std::ranges::any_of(_worldSurfaces,
                [&manifest](const WorldSurfaceInstance& a_ws) {
                    return a_ws.renderer && !a_ws.failed && a_ws.viewId == manifest.id;
                });
#else
            const bool world = false;
#endif
            if (overlay || world) {
                targets.push_back({ manifest.id, overlay, world });
            }
        }
        _devViewReload->SetTargets(std::move(targets));

        bool overlayReloaded = false;
        for (const auto& ready : _devViewReload->DrainReady()) {
            const auto* manifest = _views.Find(ready.id);
            if (!manifest) continue;
            const bool overlay = ready.overlay && _menus.IsRegistered(ready.id);
#if defined(OSFUI_WITH_WORLD_SURFACES)
            bool world = false;
#else
            const bool world = false;
#endif
            if (overlay) {
                ReloadViewInPlace(ready.id, *manifest);
                overlayReloaded = true;
            }
#if defined(OSFUI_WITH_WORLD_SURFACES)
            if (ready.world) {
                for (auto& worldSurface : _worldSurfaces) {
                    if (!worldSurface.renderer || worldSurface.failed ||
                        worldSurface.viewId != ready.id) {
                        continue;
                    }
                    world = true;
                    auto worldManifest = *manifest;
                    worldManifest.transparent = false;
                    worldManifest.width = worldSurface.width;
                    worldManifest.height = worldSurface.height;
                    worldSurface.renderer->LoadView(worldManifest);
                }
            }
#endif
            if (overlay || world) {
                REX::INFO("Runtime: dev reloaded loose view '{}'", ready.id);
            }
        }
        if (overlayReloaded) BroadcastViewsData();
    }

	bool Runtime::HudAutoStartEligible(const ViewManifest& a_manifest) const
	{
		return a_manifest.kind == SurfaceKind::Hud &&
		       !_warmViews.contains(a_manifest.id) &&
		       a_manifest.hub && (!a_manifest.debugOnly || _config.debugMode);
	}

	nlohmann::json Runtime::BuildViewsData() const
	{
		nlohmann::json views = nlohmann::json::array();
		const auto     active = _menus.ActiveMenu();
		for (const auto& m : _views.All()) {
			// Every discovered manifest is a launchable surface, so list them all.
			// A registered surface carries its live load state; a discovered-but-
			// unregistered one is reported "unloaded" so the Mods launcher can show
			// it as a click-to-load card (the click's menu.open loads it on demand
			// through EnqueueOpenView). A view whose recovery was exhausted stays
			// "failed" — its _viewLoadState entry survives the Unregister, so it is
			// caught below before the registered/unloaded split. hub:false and
			// debugOnly views are still withheld via the hub flag.
			const bool registered = _menus.IsRegistered(m.id);
			const auto state = GetViewLoadState(m.id);
			const char* loadState =
				state == ViewLoadState::Failed   ? "failed" :
				state == ViewLoadState::Finished ? "loaded" :
				registered                       ? "loading" :
				                                   "unloaded";
			// Protocol 1.6 startup-policy fields. `autoStart` is the effective
			// choice for the NEXT launch; pinned core views always run and are
			// never player-configurable.
			const bool pinned = _warmViews.contains(m.id);
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
				{ "kind", m.kind == SurfaceKind::Hud ? "hud" : "menu" },
				{ "interactive", m.interactive },
				{ "hub", m.hub && (!m.debugOnly || _config.debugMode) },
				{ "targetVersion", m.targetVersion },
				{ "open", _menus.IsOpen(m.id) },
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

	std::unordered_set<std::string> Runtime::LiveViewsOfMod(std::string_view a_mod) const
	{
		std::unordered_set<std::string> targets;
		for (const auto& manifest : _views.All()) {
			if (!_menus.IsRegistered(manifest.id)) {
				continue;
			}
			// Case-INSENSITIVE. A Papyrus mod id arrives through BSFixedString
			// interning, which hands back the first casing the process saw,
			// while a view id is lowercase by grammar. 1.x matched
			// case-sensitively here and case-insensitively on replay, so a mod
			// whose folder case differed from its script's spelling got its
			// state on reload and never on a live push.
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
		const auto targets = LiveViewsOfMod(a_mod);
		if (targets.empty()) {
			// Not an error, and not a lost write: the value is retained, so the
			// mod's first view is replayed it the moment it greets the bridge.
			REX::DEBUG("Runtime: state '{}/{}' has no live view yet — retained for the next greeting",
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
				if (_diagnostics) {
					_bridge->PublishState(a_view, "osfui", "diagnostics", _diagnostics->Snapshot());
				}
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
			if (manifest.permissions.nativeBridge && _menus.IsRegistered(manifest.id)) {
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
		for (const auto* key : { "settings", "views", "diagnostics", "i18n" }) {
			PublishPlatformState(key, a_viewId);
		}
		if (a_viewId == kHandoffViewId && !_handoffState.is_null()) {
			_bridge->PublishState(a_viewId, "osfui", "handoff", _handoffState);
		}
		// The document's own mod's retained state, from whichever backend
		// published it — Papyrus SetView* or the native ABI's SetViewState.
		const std::string mod{ Ids::ModOf(a_viewId) };
		if (const auto* entries = _viewState.Find(mod)) {
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

	void Runtime::OnProtocolMisuse(std::string_view a_viewId, std::string_view a_code,
		std::string_view a_message, const nlohmann::json& a_detail)
	{
		// devMode: hand it straight back to the offending document so it lands
		// in that view's OWN console — and therefore in F12 DevTools with full
		// object inspection, and in the SFSE log through the host's console
		// forwarder. One mechanism, both surfaces, no second channel.
		if (_config.devMode && _bridge) {
			_bridge->Emit(a_viewId, "osfui.debug.error", nlohmann::json{
				{ "code", std::string(a_code) },
				{ "message", std::string(a_message) },
				{ "detail", a_detail },
			});
		}
		// A release build has no debug channel, so REPETITION is the signal: a
		// view that keeps getting the protocol wrong earns a health card. A
		// one-off (a stale view naming one dead endpoint at boot) stays out of
		// the player's face.
		constexpr std::uint32_t kMisuseThreshold = 10;
		if (a_viewId.empty() || !_diagnostics) {
			return;
		}
		const auto count = ++_protocolMisuse[std::string(a_viewId)];
		if (count != kMisuseThreshold) {
			return;
		}
		_diagnostics->Upsert({
			.id = std::format("view.protocol-misuse:{}", a_viewId),
			.code = "view.protocol-misuse",
			.severity = DiagnosticsModule::Severity::Warning,
			// Dotless: the Mods surface reads a dot in `source` as "a mod
			// reported this", and this is the platform reporting about a view.
			.source = "views",
			.subject = std::string(a_viewId),
			.context = nlohmann::json{ { "code", std::string(a_code) }, { "count", count } },
		}, _uptime);
		_diagnostics->Broadcast();
	}

	Runtime::ViewLoadState Runtime::GetViewLoadState(std::string_view a_id) const
	{
		const auto it = _viewLoadState.find(std::string(a_id));
		return it == _viewLoadState.end() ? ViewLoadState::Loading : it->second;
	}

	void Runtime::DriveRendererHostRecovery()
	{
		if (_rendererHostRecovery.ExpireResponseWait(_uptime)) {
			REX::ERROR("Runtime: replacement WebView2 helper produced no load response in {:.0f}s",
				RendererHostRecovery::kResponseTimeoutSeconds);
			if (_rendererHostRecovery.PhaseValue() ==
				RendererHostRecovery::Phase::Exhausted) {
				REX::ERROR("Runtime: automatic WebView2 helper recovery exhausted; "
						   "the next explicit menu open will start a fresh retry cycle");
			}
		}

		if (!_rendererHostRecovery.BeginDueAttempt(_uptime)) {
			return;
		}

		const auto attempt = _rendererHostRecovery.Attempts();
		REX::INFO("Runtime: restarting WebView2 helper (attempt {}/{})",
			attempt, RendererHostRecovery::kMaxAttempts);
		if (!_renderer || !_renderer->RestartAfterFailure()) {
			REX::ERROR("Runtime: renderer could not reset its failed host connection");
			_rendererHostRecovery.OnAttemptSetupFailed(_uptime);
			if (_rendererHostRecovery.PhaseValue() ==
				RendererHostRecovery::Phase::Exhausted) {
				REX::ERROR("Runtime: automatic WebView2 helper recovery exhausted; "
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
		_readyViews.clear();
		_gamepadRawViews.clear();
		_backOwnerViews.clear();
		_pendingMouseMove.store(kNoPendingMouseMove);
		_lastSubmittedFrame = 0;
		_renderStatsHaveBaseline = false;
		_nativeFocusGranted = false;
		_viewLifecycle.OnHostRestart(_uptime);

		std::size_t reloaded = 0;
		for (const auto& manifest : _views.All()) {
			if (!_menus.IsRegistered(manifest.id)) {
				continue;
			}
			_viewLoadState[manifest.id] = ViewLoadState::Loading;
			_renderer->LoadView(manifest);
			_renderer->SetRenderStats(manifest.id, _renderStatsEnabled);
			if (manifest.permissions.nativeBridge) {
				// RestartAfterFailure discarded messages addressed to the dead
				// documents. Each replacement greets the bridge on load and is
				// replayed then; all this has to do is re-arm its gate.
				_bridge->OnViewCreated(manifest.id);
			}
			++reloaded;
		}

		for (const auto& id : _warmViews) {
			if (_menus.IsRegistered(id)) {
				_renderer->PrewarmView(id);
			}
		}
		_renderer->Resize(_viewWidth.load(), _viewHeight.load());
		_renderer->SetAcceleratorKeys(_toggleKey.load(std::memory_order_acquire),
			false, _captureArmed.load(), _captureUpVk.load());
		ApplyMenuPolicy();
		BroadcastViewsData();
		REX::INFO("Runtime: replayed {} registered view(s) to the replacement helper; "
				  "overlay left closed", reloaded);
	}

	void Runtime::OnRendererFailure(const IWebRenderer::FailureEvent& a_event)
	{
		if (_rendererFailureLatched) {
			return;
		}
		_rendererFailureLatched = true;
		_rendererFailed = true;
		const bool retryableHostLoss =
			a_event.stage == "host-connection" && _renderer && _renderer->Name() == "webview2";
		if (retryableHostLoss) {
			_rendererHostRecovery.OnRetryableFailure(_uptime);
			REX::ERROR("Runtime: WebView2 helper connection failed for view '{}' (0x{:08X}): {} - "
					   "closing the overlay; bounded helper recovery is scheduled",
				a_event.viewId, a_event.errorCode, a_event.description);
			if (_rendererHostRecovery.PhaseValue() ==
				RendererHostRecovery::Phase::Exhausted) {
				REX::ERROR("Runtime: automatic WebView2 helper recovery exhausted; "
						   "the next explicit menu open will start a fresh retry cycle");
			}
		} else {
			_rendererHostRecovery.Disable();
			REX::ERROR("Runtime: renderer failed at '{}' for view '{}' (0x{:08X}): {} - "
					   "closing the overlay and disabling it for this session",
				a_event.stage, a_event.viewId, a_event.errorCode, a_event.description);
		}
		_recovery.clear();

		CancelPendingOpen();
		_menus.CloseAll();
		ApplyMenuPolicy();

		// The fatal callback arrives from renderer Update(), after Tick's normal
		// policy reconciliation. Release every engine-side effect now instead of
		// leaving actors, controls, pause, or the cursor stranded for another frame.
		if (_config.focusMenu) {
			ReconcileFocusMenu();
		}
		ReconcileControlLayer();
		ReconcileSimPause();
		FreeCursor::Apply(false);
	}

	bool Runtime::IsInputCaptured() const
	{
		return _initialized && _captureInput.load() && _visible.load();
	}

	void Runtime::ApplyToggleKey()
	{
		const auto toggleKey = _toggleKey.load(std::memory_order_acquire);
		if (!_inputConfigured) {
			_input.Configure(
				toggleKey,
				[this] { EnqueueMenuRequest(MenuReq::ToggleDefault); },
				[this] { EnqueueMenuRequest(MenuReq::Back); });
			_inputConfigured = true;
		} else {
			_input.SetToggleKey(toggleKey);
		}
	}

	void Runtime::NotifyPlayerCloseRequest()
	{
		if (_renderer) {
			_renderer->NotifyPlayerCloseRequest();
		}
	}

	bool Runtime::OnHostKey(std::uint32_t a_vkCode, bool a_down)
	{
		// Key-rebind capture (armed by settings.captureKey). Grab the next key
		// press and consume it, so pressing the current toggle key (or Esc)
		// rebinds instead of closing the overlay. Only stash the VK here; the
		// apply happens on the main thread in DrainKeyCapture. The matching key-up
		// is swallowed too so it can't leak/route.
		if (_captureArmed.load()) {
			if (a_down) {
				_capturedVk.store(a_vkCode);
				_captureArmed.store(false);
				_captureUpVk = a_vkCode;
			}
			return true;
		}
		const auto captureUpVk = _captureUpVk.load();
		if (captureUpVk != kInvalidKeyCode && a_vkCode == captureUpVk && !a_down) {
			_captureUpVk = kInvalidKeyCode;
			return true;
		}

		// F12 opens WebView2 DevTools for the top open menu in devMode. Only raise
		// a flag here: renderer IPC belongs on the main tick.
		if (_config.devMode && a_vkCode == kVkF12) {
			if (a_down) {
				_devToolsRequested.store(true);
			}
			return true;
		}

		// Hotkey dispatch (mcm-design.md §9): a key-down edge may fire mods'
		// key-typed bindings. The service self-suppresses while the overlay
		// captures input or a rebind is armed (belt and braces — the armed path
		// above already returned); fires queue here on the window thread and
		// deliver from Tick (DrainHotkeys). Does not consume: the game (and the
		// toggle/router path below) still sees the key.
		if (a_down) {
			_hotkeys.OnKeyDown(a_vkCode);
		}

		// Decide consumption before routing: capturing or the toggle key must not
		// reach the game (the toggle press itself is consumed so opening the
		// overlay never also acts in-game).
		const auto toggleKey = _toggleKey.load(std::memory_order_acquire);
		const bool consume = IsInputCaptured() || a_vkCode == toggleKey;
		if (a_down) {
			_input.OnKeyDown(a_vkCode);
		} else {
			_input.OnKeyUp(a_vkCode);
		}
		return consume;
	}


	bool Runtime::OnNativeAcceleratorKey(std::uint32_t a_vkCode, bool a_down)
	{
		const bool frameworkOwned =
			_captureArmed.load() ||
			(_captureUpVk.load() != kInvalidKeyCode && a_vkCode == _captureUpVk.load()) ||
			a_vkCode == _toggleKey.load(std::memory_order_acquire) ||
			(_config.devMode && a_vkCode == kVkF12) ||
			(a_vkCode == 0x1B && IsInputCaptured());
		return frameworkOwned && OnHostKey(a_vkCode, a_down);
	}

	void Runtime::OnHostMouseAbsolute(int a_clientX, int a_clientY, int a_clientW, int a_clientH)
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

	void Runtime::OnHostMouseDelta(int a_dx, int a_dy)
	{
		if (!IsInputCaptured() || !_renderer) {
			return;
		}
		// Scale raw deltas so a view sweep costs the same physical travel at any
		// resolution; the view tracks the screen, so 1:1 would feel slow when big.
		const auto scale = _cursorScale.load(std::memory_order_relaxed);
		const auto maxX = static_cast<float>(_viewWidth.load(std::memory_order_relaxed) - 1);
		const auto maxY = static_cast<float>(_viewHeight.load(std::memory_order_relaxed) - 1);
		const auto addClamped = [](std::atomic<float>& a_value, const float a_delta, const float a_max) {
			auto current = a_value.load(std::memory_order_relaxed);
			for (;;) {
				const auto next = std::clamp(current + a_delta, 0.0f, a_max);
				if (a_value.compare_exchange_weak(current, next,
						std::memory_order_relaxed, std::memory_order_relaxed)) {
					break;
				}
			}
		};
		addClamped(_cursorX, static_cast<float>(a_dx) * scale, maxX);
		addClamped(_cursorY, static_cast<float>(a_dy) * scale, maxY);
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

	void Runtime::OnHostMouseButton(int a_button, bool a_down)
	{
		if (!IsInputCaptured() || !_renderer) {
			return;
		}
		_renderer->InjectMouseButton(
			static_cast<int>(_cursorX.load(std::memory_order_relaxed)),
			static_cast<int>(_cursorY.load(std::memory_order_relaxed)), a_button, a_down);
	}

	void Runtime::OnHostMouseWheel(int a_wheelDelta)
	{
		if (!IsInputCaptured() || !_renderer) {
			return;
		}
		// Route at the current virtual cursor; the renderer forwards the raw
		// delta to the host's WebView2 WHEEL input, which performs the scroll.
		_renderer->InjectPhysicalMouseWheel(
			static_cast<int>(_cursorX.load(std::memory_order_relaxed)),
			static_cast<int>(_cursorY.load(std::memory_order_relaxed)), a_wheelDelta);
	}

	void Runtime::ReconcileFocusMenu()
	{
		// Main thread (Runtime::Tick). Drive the engine menu's open state toward
		// the top menu's capture policy. Pause is not wired through this menu's
		// flags
		// (the real pause flag, bit 1, would tie pause to capture instead of the
		// per-view pausesGame policy) — sim pause is ReconcileSimPause. Act only
		// on a change, to avoid per-frame queue spam.
		const bool wantOpen = _menus.DesiredCapture();
		if (wantOpen != _focusMenuOpen) {
			_focusMenuOpen = wantOpen;
			_focusMenuMismatchSince = -1.0;  // fresh request: full grace window
			if (wantOpen) {
				FocusMenu::Open();
			} else {
				FocusMenu::Close();
				// Clear the gamepad routing queue/sticks (no-op unless engineInput).
				EngineInput::ResetSessionRouting();
				// Gamepad raw-passthrough is not reset here: it is a sticky
				// per-view property (_gamepadRawViews) that survives overlay
				// hide/show. Another menu opening can't inherit it, because
				// DrainEngineInput reads the active view's flag each tick.
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
		// engine menu (UI::ModifyMenuPauseCounter; see input/SimPause), so it is
		// not gated on config.focusMenu. Driven by the top menu's manifest
		// pausesGame (default true for menus). Edge-triggered inside Apply.
		SimPause::Apply(_menus.DesiredPause());
	}

	void Runtime::DrainEngineInput(double a_deltaSeconds)
	{
		if (!_renderer) {
			return;
		}
		const bool captured = IsInputCaptured();
		const auto active = _menus.ActiveMenu();
		// Raw mode is the active view's sticky flag — per view, so menu switches
		// can't leak one page's grant to another. The EngineInput global mirrors
		// it, keeping the mode-flip log in one place.
		const bool raw = active && _gamepadRawViews.contains(*active);
		EngineInput::SetRawMode(raw);
		// While capturing, the receiver thunks consume gamepad events after
		// recording them (status=kStop): the ControlLayer disable flags do not
		// gate thumbstick movement, so without this the player walks around
		// under the open overlay. Tracks capture, not visibility — a live HUD
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
			case XInputButton::kB:         EnqueueMenuRequest(MenuReq::Back); break;  // back — delegate (osfui.handleBack) or close
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
		// game underneath a capturing menu. A live HUD (no capture) leaves
		// controls enabled. Apply edge-detects internally and retries until the
		// manager exists.
		ControlLayer::Apply(_menus.DesiredCapture());
	}

	void Runtime::BuildModules()
	{
		// Settings: schemas ship read-only under <data>/settings/*.json; values
		// persist per-mod under <data>/settings/values — in the Data tree, not
		// Documents, because under MO2 the write is VFS-captured (Overwrite), so
		// settings are per-profile, travel with instance backups, and sit next to
		// the mod (MCM-Helper precedent; mcm-design.md §8.1).
		const auto schemaDir = Paths::DataDir() / "settings";
		const auto valuesDir = schemaDir / "values";
		auto settings = std::make_unique<SettingsModule>(schemaDir, valuesDir,
			[this](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
				OnSettingChanged(a_mod, a_key, a_value);
			});
		_settings = settings.get();  // core needs schema facts (e.g. key-capture gating)
		_settings->Store().SetTextResolver([this](std::string_view a_mod, std::string_view a_address, std::string_view a_english) {
			return _localization.Resolve(a_mod, a_address, a_english);
		});

		// ABI feed (mcm-design.md §8.2): every committed value — including the
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
			// Papyrus change callbacks (mcm-design.md §8.4), after the mirror
			// update: the dispatched script call reads current values through the
			// mirror-backed getters, so the mirror must never lag it.
			API::Papyrus::OnSettingChanged(a_mod, a_key);
		});
		store.AddRegistryListener([this] {
			if (_settings) {  // teardown guard (_settings nulls before modules die)
				API::BridgeApi::Get().Mirror().Rebuild(_settings->Store().DataView());
			}
		});

		// HotkeyService (mcm-design.md §9): every key-typed setting is a live
		// binding. The registry rebuilds on any key-typed commit (web, ABI or
		// reset) and on registry shape change; the store's conflict grouping shares
		// this key-name resolution, so the store stays input-agnostic. Suppression
		// reads the same capture state OnHostKey consults, so a press while the
		// user types in a settings field or mid-rebind cannot fire a hotkey.
		store.SetKeyNameResolver(ResolveKeyName);

		// Vanilla hotkeys (mcm-design.md §9) are not loaded here: the
		// osfui.vanillaKeyConflicts setting is MCM-owned, so the OnStart NotifyAll
		// replay drives ApplyVanillaKeyConflicts with the persisted value (default
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

		_modules.push_back(std::move(settings));

		// System Health (bridge protocol 1.4): a session-scoped registry every
		// subsystem reports durable, actionable conditions to. Deliberately
		// LAST, so a producer that fires during another module's OnStart finds
		// the registry already constructed.
		auto diagnostics = std::make_unique<DiagnosticsModule>();
		_diagnostics = diagnostics.get();
		_modules.push_back(std::move(diagnostics));

		REX::INFO("Runtime: {} UI module(s) loaded", _modules.size());
	}

	void Runtime::DrainKeyCapture()
	{
		const KeyCode vk = _capturedVk.exchange(kInvalidKeyCode);
		if (vk == kInvalidKeyCode) {
			return;  // nothing captured this tick
		}
		if (!_bridge || _captureView.empty()) {
			return;  // nobody to answer
		}
		// Escape cancels the rebind; an unnameable VK can't be a binding.
		constexpr KeyCode kVkEscape = 0x1B;
		const std::string name = (vk == kVkEscape) ? std::string{} : KeyName(vk);
		const bool cancelled = name.empty();
		// Tell the view which setting + the captured name; it echoes back a normal
		// settings.set, so the store persists and OnSettingChanged re-resolves.
		nlohmann::json payload{
			{ "mod", _captureMod },
			{ "key", _captureKey },
			{ "name", name },
			{ "cancelled", cancelled },
		};
		// Live-warn during capture (mcm-design.md §9): which other key-typed
		// settings already sit on this key, so the UI warns before the view
		// commits. The store still holds this setting's old binding (the commit is
		// the view's echo), so exclude self. Informational, never blocking.
		if (!cancelled && _settings) {
			if (auto conflicts = _settings->Store().ConflictsFor(vk, _captureMod, _captureKey); !conflicts.empty()) {
				payload["conflicts"] = std::move(conflicts);
			}
		}
		// A one-shot happening, so it is an EVENT, not the arming request's
		// reply: `settings.captureKey` already settled in machine time with
		// "armed". Requests settle in machine time; human-time outcomes are
		// events (docs/mod-api-2.0-design.md, "User-paced flows settle fast").
		_bridge->Emit(_captureView, "settings.captured", payload);
		REX::DEBUG("Runtime: key capture -> {} (VK {:#04x}) ({}.{})",
			cancelled ? "(cancelled)" : name, vk, _captureMod, _captureKey);
		_captureView.clear();
		_captureMod.clear();
		_captureKey.clear();
		// The capture is answered; stop swallowing the captured key's release.
		// A letter/digit VK never reaches the accelerator hook on key-up, so
		// without this the latch stays armed and eats that key's next release
		// in gameplay. The dangerous ups (Esc, the toggle key) are still owned
		// by their own conditions in OnNativeAcceleratorKey.
		_captureUpVk = kInvalidKeyCode;
	}

	void Runtime::CancelArmedKeyCapture()
	{
		if (!_captureArmed.exchange(false)) {
			return;
		}
		_captureUpVk = kInvalidKeyCode;
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
		// Gameplay gate (mcm-design.md §9): a press while a game menu is up
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
			// Delivery channels (mcm-design.md §9): C ABI subscribers (queued
			// here, invoked unlocked by BridgeApi::PumpMainThread later this
			// tick) and the web `ui.hotkey` push to settings subscribers.
			API::BridgeApi::Get().Hotkeys().OnFired(a_mod, a_key);
			if (_settings) {
				_settings->PushHotkey(a_mod, a_key);
			}
			// Third channel (mcm-design.md §8.4): registered Papyrus callbacks,
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
						_runtimeDiagnostics.ResolveHotkeyTarget(a_mod, a_key);
					} else {
						const auto reason = result == API::Papyrus::StaticDispatchResult::kVmUnavailable ?
							"the Papyrus VM is unavailable" :
							"Papyrus rejected the call; the script may be missing, the function may be absent "
							"or non-GLOBAL, or its signature may not be (string, string)";
						_runtimeDiagnostics.ReportHotkeyTargetFailure(
							a_mod, a_key, target->script, target->function, reason);
					}
				} else {
					_runtimeDiagnostics.ResolveHotkeyTarget(a_mod, a_key);
				}
			}
			REX::DEBUG("Runtime: hotkey fired for {}.{}", a_mod, a_key);
		});
	}

	void Runtime::DrainBugReportResult()
	{
		std::optional<BugReportResult> result;
		{
			std::scoped_lock lock(_bugReportMutex);
			if (_bugReportResult) {
				result = std::move(_bugReportResult);
				_bugReportResult.reset();
			}
		}
		if (!result) return;
		_bugReportInFlight.store(false, std::memory_order_release);
		if (!_bridge) return;

		nlohmann::json payload{ { "ok", result->ok } };
		if (!result->code.empty()) payload["code"] = result->code;
		if (!result->message.empty()) payload["message"] = result->message;
		if (!result->reportId.empty()) payload["reportId"] = result->reportId;
		if (result->issueNumber != 0) payload["issueNumber"] = result->issueNumber;
		// Settles the request the submit deferred. A failed upload REJECTS with
		// its code rather than resolving an { ok:false } document the caller has
		// to remember to inspect.
		if (result->ok) {
			_bridge->RespondTo(result->requestId, payload);
		} else {
			_bridge->RejectTo(result->requestId,
				result->code.empty() ? "report-failed" : result->code, result->message);
		}
	}

	void Runtime::RegisterPlatformCommands(MessageBridge& a_bridge)
	{
		// The platform owns only window/diagnostic endpoints. Features register
		// their own; there is no generic "call native" escape hatch.
		//
		// The kind of each endpoint is chosen by ONE question: does the caller
		// need a completion? A dismissal cannot meaningfully fail, so `close` is
		// a send; opening a surface by id can name a view that does not exist,
		// so `menu.open` is a request. Reads-with-replay are neither — the four
		// registries a view used to `*.get` (settings, views, diagnostics, i18n)
		// are published as state instead, which is what makes them survive F5
		// with no lifecycle code in the view.
		a_bridge.RegisterSend("close", [this](const nlohmann::json&, MessageBridge& a_b) {
			if (a_b.CurrentSource() == kHandoffViewId && CancelPendingOpen()) {
				ApplyMenuPolicy();
				return;
			}
			// Dismiss the calling surface. Closing the last open menu empties the
			// stack, so the overlay hides; a coexisting live HUD stays up.
			if (_menus.Close(a_b.CurrentSource())) {
				ApplyMenuPolicy();
			}
		});
		a_bridge.RegisterSend("setVisible", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string src(a_b.CurrentSource());
			const bool changed = Json::GetBool(a_p, "visible", false) ? _menus.Open(src) : _menus.Close(src);
			if (changed) {
				ApplyMenuPolicy();
			}
		});
		// Open/close a surface by id (defaults to the calling view). menu.* and
		// hud.* are aliases: a surface's kind is fixed by its manifest, not by the
		// command used.
		const auto surfaceOpen = [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			std::string id = Json::GetString(a_p, "view", "");
			if (id.empty()) {
				id = std::string(a_b.CurrentSource());
			}
			if (!_views.Find(id)) {
				REX::WARN("Runtime: menu.open refused — '{}' was not discovered", id);
				a_b.Reject("unknown-view", "view was not discovered");
				return;
			}
			// Use the same snapshot/load/pump/open path as native RequestMenu so a
			// discovered surface is created while hidden on the next tick. The
			// reply means "accepted and queued", which is all the caller can act
			// on — the open itself lands on the next tick.
			EnqueueOpenView(std::move(id));
			a_b.Respond(nlohmann::json::object());
		};
		const auto surfaceClose = [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			std::string id = Json::GetString(a_p, "view", "");
			if (id.empty()) {
				id = std::string(a_b.CurrentSource());
			}
			bool cancelled = false;
			if (_pendingSurfaceOpen &&
				(_pendingSurfaceOpen->target == id || id == kHandoffViewId)) {
				cancelled = CancelPendingOpen();
			}
			if (_menus.Close(id)) {
				ApplyMenuPolicy();
			} else if (cancelled) {
				ApplyMenuPolicy();
			} else if (!_menus.IsRegistered(id)) {
				a_b.Reject("unknown-view", "not a registered surface");
				return;
			}
			// Already closed = the desired state was reached.
			a_b.Respond(nlohmann::json::object());
		};
		// `hud.show`/`hud.hide` are gone: they were bound to these very lambdas,
		// so they were four names for two behaviors. A surface's kind is fixed by
		// its manifest, not by the endpoint the page happened to pick.
		a_bridge.RegisterRequest("menu.open", surfaceOpen);
		a_bridge.RegisterRequest("menu.close", surfaceClose);
		a_bridge.RegisterSend("view.ready", [this](const nlohmann::json&, MessageBridge& a_b) {
			const std::string source(a_b.CurrentSource());
			const auto* manifest = _views.Find(source);
			if (!manifest || !manifest->permissions.nativeBridge) {
				// Unreachable in practice — a view without nativeBridge has no
				// bridge to send through — so surface it rather than answering.
				a_b.Surface(source, "forbidden", "view.ready requires nativeBridge");
				return;
			}
			_readyViews.insert(source);
			REX::DEBUG("Runtime: view '{}' declared meaningful readiness", source);
		});
		a_bridge.RegisterSend("osfui.handoffRetry", [this](const nlohmann::json&, MessageBridge& a_b) {
			if (a_b.CurrentSource() != kHandoffViewId) {
				a_b.Surface(a_b.CurrentSource(), "forbidden", "osfui.handoffRetry is a platform action");
				return;
			}
			RetryPendingOpen();
		});
		a_bridge.RegisterRequest("setViewHidden", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			// Show/hide one loaded view by id, independent of the overlay toggle.
			// Omitting "view" targets the calling view (self-hide).
			std::string id = Json::GetString(a_p, "view", "");
			if (id.empty()) {
				id = std::string(a_b.CurrentSource());
			}
			if (!SetViewHidden(id, Json::GetBool(a_p, "hidden", false))) {
				a_b.Reject("unknown-view", "not a loaded view");
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
		// "waiting for you" indistinguishable from "the backend died".
		// Any schema-declared `type:"key"` setting is rebindable — the schema
		// gates the capture, not an allowlist.
		// Main thread; OnHostKey (window thread) reads the armed flag.
		a_bridge.RegisterRequest("settings.captureKey", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			const auto requestedMod = Json::GetString(a_p, "mod", "");
			// A capture ends in a settings write, so it carries the same authority
			// requirement (Ids::ResolveWritableMod): only the built-in Mods surface
			// and keybinds board may rebind another mod's keys.
			const auto allowedMod = Ids::ResolveWritableMod(a_b.CurrentSource(), requestedMod);
			if (!allowedMod) {
				REX::WARN("Runtime: [content] view '{}' refused settings.captureKey for '{}' (not its own mod)",
					a_b.CurrentSource(), requestedMod);
				a_b.Reject("forbidden", "a view may only rebind its own mod's keys");
				return;
			}
			const std::string mod(*allowedMod);
			const std::string key = Json::GetString(a_p, "key", "");
			// One capture at a time: a second arm while one is live is refused
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
			const auto it = a_p.find("args");
			if (it == a_p.end() || !it->is_array()) {
				return args;
			}
			args.reserve(it->size());
			for (const auto& e : *it) {
				if (e.is_string()) {
					args.push_back(e.get<std::string>());
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
			const std::string script = Json::GetString(a_p, "script", "");
			const std::string function = Json::GetString(a_p, "function", "");
			if (!PapyrusNames::IsScriptName(script) || !PapyrusNames::IsIdentifier(function)) {
				a_b.Surface(source, "invalid-request", "papyrus.call requires valid 'script' and 'function' names");
				return;
			}
			const auto it = a_p.find("args");
			if (it != a_p.end() && !it->is_array()) {
				a_b.Surface(source, "invalid-request", "papyrus.call 'args' must be an array");
				return;
			}
			const auto& input = it == a_p.end() ? nlohmann::json::array() : *it;
			if (input.size() > 32) {
				a_b.Surface(source, "invalid-request", "papyrus.call accepts at most 32 arguments");
				return;
			}
			std::vector<API::Papyrus::StaticCallArg> args;
			args.reserve(input.size());
			const auto appendFloat = [&](double number) {
				if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max()) {
					return false;
				}
				args.emplace_back(static_cast<float>(number));
				return true;
			};
			for (const auto& value : input) {
				if (value.is_string()) {
					args.emplace_back(value.get<std::string>());
				} else if (value.is_boolean()) {
					args.emplace_back(value.get<bool>());
				} else if (value.is_number_integer()) {
					const auto integer = value.get<std::int64_t>();
					if (integer < std::numeric_limits<std::int32_t>::min() || integer > std::numeric_limits<std::int32_t>::max()) {
						a_b.Surface(source, "invalid-request", "papyrus.call integer arguments must fit Papyrus int");
						return;
					}
					args.emplace_back(static_cast<std::int32_t>(integer));
				} else if (value.is_number_unsigned()) {
					const auto integer = value.get<std::uint64_t>();
					if (integer > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
						a_b.Surface(source, "invalid-request", "papyrus.call integer arguments must fit Papyrus int");
						return;
					}
					args.emplace_back(static_cast<std::int32_t>(integer));
				} else if (value.is_number_float()) {
					if (!appendFloat(value.get<double>())) {
						a_b.Surface(source, "invalid-request", "papyrus.call float arguments must be finite Papyrus floats");
						return;
					}
				} else if (value.is_object() && value.size() == 2 &&
					Json::GetString(value, "$papyrus", "") == "float" && value.contains("value") &&
					value["value"].is_number() && appendFloat(value["value"].get<double>())) {
					// JSON erases the difference between 3 and 3.0. The helper's tagged
					// float keeps whole-valued Papyrus float parameters expressible.
				} else {
					a_b.Surface(source, "invalid-request", "papyrus.call arguments must be scalar values or osfui.papyrus.float(value)");
					return;
				}
			}
			if (API::Papyrus::DispatchStaticFunction(script, function, args) !=
				API::Papyrus::StaticDispatchResult::kQueued) {
				a_b.Surface(source, "papyrus-unavailable", "Papyrus could not queue the GLOBAL function");
			}
		});
		a_bridge.RegisterSend("papyrus.send", [papyrusArgs](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string source(a_b.CurrentSource());
			const std::string mod{ Ids::ModOf(source) };
			const std::string name = Json::GetString(a_p, "name", "");
			if (name.empty()) {
				a_b.Surface(source, "invalid-request", "papyrus.send requires a non-empty 'name'");
				return;
			}
			API::Papyrus::OnViewAction(mod, name, papyrusArgs(a_p));
		});
		a_bridge.RegisterRequest("papyrus.request", [papyrusArgs](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string source(a_b.CurrentSource());
			const std::string mod{ Ids::ModOf(source) };
			const std::string name = Json::GetString(a_p, "name", "");
			if (mod.empty() || name.empty() || name.size() > 64) {
				a_b.Reject("invalid-request", "name must be a non-empty string of at most 64 characters");
				return;
			}
			const bool queued = API::Papyrus::OnViewRequest(
				mod, name, papyrusArgs(a_p), source, a_b.CurrentRequestId());
			if (!queued) {
				a_b.Reject("papyrus-unavailable", "no Papyrus request listener is available");
				return;
			}
			// The script settles it later through ReplyView*/RejectViewRequest.
			a_b.Defer();
		});
		a_bridge.RegisterSend("log", [](const nlohmann::json& a_p, MessageBridge&) {
			// Untrusted content: bound the length so JS cannot flood the log.
			REX::DEBUG("MessageBridge: [web] {}", Json::GetString(a_p, "text", "").substr(0, 512));
		});
		a_bridge.RegisterRequest("ping", [](const nlohmann::json&, MessageBridge& a_b) {
			a_b.Respond(nlohmann::json::object());
		});
		a_bridge.RegisterSend("osfui.gamepadRaw", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			// A page that wants to own the gamepad (e.g. stick-driven camera orbit)
			// sets this to suppress the default nav/scroll mapping and handle raw
			// `ui.gamepad` events itself. Sticky per view: survives overlay
			// hide/show, clears on page reload or view destroy. DrainEngineInput
			// applies the active view's flag each tick.
			const std::string src(a_b.CurrentSource());
			if (src.empty()) {
				return;
			}
			if (Json::GetBool(a_p, "raw", false)) {
				_gamepadRawViews.insert(src);
			} else {
				_gamepadRawViews.erase(src);
			}
		});
		// `osfui.textFocus` is gone. It was registered as a no-op purely so a
		// pre-session-focus view would not trip `unknown-command`; an unknown
		// send is now a dev-only debug event, so the placeholder bought nothing.
		a_bridge.RegisterRequest("osfui.openModPage", [](const nlohmann::json&, MessageBridge& a_b) {
			// "Update OSF UI" affordances in views (e.g. OSF Animation's status-line
			// UPDATE badge): open OSF UI's own Nexus page in the SYSTEM browser —
			// the overlay itself must never navigate, and the URL is a compile-time
			// constant precisely so page content cannot steer the shell (the
			// payload carries nothing). Behind a fullscreen game the browser opens
			// unfocused; alt-tab surfaces it.
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
			// System Health's "Open log folder" action (protocol 1.4). The twin
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
			// Startup policy is player intent: only the built-in Mods surface may
			// change it — the same exact-id gate the diagnostics.* requests use.
			if (a_b.CurrentSource() != kSettingsViewId) {
				a_b.Reject("forbidden", "view auto-start is set from OSF UI's built-in settings view");
				return;
			}
			const auto view = Json::GetString(a_p, "view", "");
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
		a_bridge.RegisterRequest("diagnostics.reportStatus", [this](const nlohmann::json&, MessageBridge& a_b) {
			if (a_b.CurrentSource() != "osfui/settings") {
				a_b.Reject("forbidden", "bug reporting is restricted to OSF UI's built-in settings view");
				return;
			}
			a_b.Respond({
				{ "enabled", _config.bugReporting },
				{ "logs", nlohmann::json::array({ "OSF UI.log", "OSF UI.webview2-host.log" }) },
				{ "retentionDays", 30 },
			});
		});
		a_bridge.RegisterRequest("diagnostics.submitReport", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			if (a_b.CurrentSource() != "osfui/settings") {
				a_b.Reject("forbidden", "bug reporting is restricted to OSF UI's built-in settings view");
				return;
			}
			if (!_config.bugReporting) {
				a_b.Reject("reporting-disabled", "bug reporting is turned off in OSF UI's settings");
				return;
			}
			// The upload runs on a worker; this id settles the request whenever
			// it finishes (PumpBugReport). A request always carries an id, so
			// there is no "was this correlated?" case left to check.
			const auto requestId = std::string(a_b.CurrentRequestId());
			// Codepoint-boundary caps. A byte cut here would hand Submit() a string
			// that is already exactly at the limit, so its own bound is a no-op and
			// the split sequence reaches the payload dump.
			const auto bounded = [](std::string a_text, std::size_t a_maxBytes) {
				StringUtil::TruncateUtf8(a_text, a_maxBytes);
				return a_text;
			};
			const auto title = bounded(Json::GetString(a_p, "title", ""), 120);
			const auto description = bounded(Json::GetString(a_p, "description", ""), 6000);
			const auto reproduction = bounded(Json::GetString(a_p, "reproduction", ""), 4000);
			if (title.empty() || description.empty()) {
				a_b.Reject("invalid-report", "title and description are required");
				return;
			}
			if (_bugReportInFlight.exchange(true, std::memory_order_acq_rel)) {
				a_b.Reject("report-busy", "another report is already being submitted");
				return;
			}
			// The source id authenticates which hosted document sent the command,
			// but built-in view files are mod-managed assets and can be replaced.
			// A native, default-No prompt is therefore the final authority for
			// transmitting logs; page-side disclosure is helpful UX, not trust.
			if (!Platform::ConfirmBugReportUpload(title)) {
				_bugReportInFlight.store(false, std::memory_order_release);
				a_b.Reject("consent-declined", "the diagnostic upload was cancelled");
				return;
			}
			const std::string endpoint = kBugReportEndpoint;
			const auto diagnostics = _diagnostics ? _diagnostics->Snapshot() : nlohmann::json::object();
			const auto view = std::string(a_b.CurrentSource());
			try {
				_bugReportWorker = std::jthread([this, endpoint, diagnostics, title,
					description, reproduction, view, requestId] {
					// The whole body is guarded: an escaping exception on a jthread
					// is a std::terminate, and it would also leave
					// _bugReportInFlight latched (only the result drain at
					// PumpBugReport clears it), so the reporter would answer
					// "report-busy" forever after. Publish a failure instead.
					BugReportResult result{ .view = view, .requestId = requestId };
					try {
						const auto submitted = Reporting::Submit(endpoint, diagnostics,
							title, description, reproduction);
						result.ok = submitted.ok;
						result.code = submitted.code;
						result.message = submitted.message;
						result.reportId = submitted.reportId;
						result.issueNumber = submitted.issueNumber;
					} catch (const std::exception& e) {
						REX::ERROR("Runtime: bug-report upload threw: {}", e.what());
						result.ok = false;
						result.code = "internal";
						result.message = "the report upload failed unexpectedly";
					} catch (...) {
						REX::ERROR("Runtime: bug-report upload threw a non-std exception");
						result.ok = false;
						result.code = "internal";
						result.message = "the report upload failed unexpectedly";
					}
					std::scoped_lock lock(_bugReportMutex);
					_bugReportResult = std::move(result);
				});
			} catch (const std::exception& e) {
				_bugReportInFlight.store(false, std::memory_order_release);
				REX::ERROR("Runtime: could not start bug-report worker: {}", e.what());
				a_b.Reject("internal", "could not start the report upload");
				return;
			}
			a_b.Defer();
		});
		a_bridge.RegisterRequest("osfui.openReportIssue", [](const nlohmann::json& a_p, MessageBridge& a_b) {
			if (a_b.CurrentSource() != "osfui/settings") {
				a_b.Reject("forbidden", "open report issue is a platform action");
				return;
			}
			const auto number = Json::GetInt(a_p, "issueNumber", 0);
			if (number <= 0 || number > 1000000000) {
				a_b.Reject("invalid-issue", "invalid report issue number");
				return;
			}
			const auto url = std::format(L"https://github.com/ozooma10/osf-ui/issues/{}", number);
			if (!Platform::OpenSystemBrowser(url.c_str())) {
				a_b.Reject("shell-failed", "could not open the system browser");
				return;
			}
			a_b.Respond(nlohmann::json::object());
		});
		a_bridge.RegisterSend("osfui.handleBack", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			// A page that owns back navigation (e.g. a sub-menu whose Esc should
			// return to the hub, not dismiss the overlay) sets this; while it is
			// the active menu, Esc / pad-B arrive as a synthetic Escape
			// keydown/keyup instead of closing the top menu. Same lifecycle as
			// osfui.gamepadRaw. The toggle key still closes natively, so this
			// cannot strand the user.
			const std::string src(a_b.CurrentSource());
			if (src.empty()) {
				return;
			}
			if (Json::GetBool(a_p, "handle", false)) {
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
		// theirs to react to. Invoked from main-thread Tick as settings commands
		// dispatch, plus once per value at startup via NotifyAll, so persisted
		// choices apply on boot.
		if (a_modId != "osfui") {
			return;
		}
		// Toggle key rebind: re-resolve and re-apply to the input router. An
		// unresolvable name keeps the working key rather than disabling the toggle.
		if (a_key == "toggleKey" && a_value.is_string()) {
			const auto name = a_value.get<std::string>();
			const auto vk = ResolveKeyName(name);
			if (vk == kInvalidKeyCode) {
				REX::WARN("Runtime: setting osfui.toggleKey '{}' is not a resolvable key; keeping '{}'", name, _config.toggleKey);
				return;
			}
			_config.toggleKey = name;
			_toggleKey.store(vk, std::memory_order_release);
			ApplyToggleKey();
			REX::INFO("Runtime: setting osfui.toggleKey -> {} (VK {:#x})", name, vk);
		}
		// Pause-menu entry (MCM-owned). The Scaleform inject runs per pause-menu
		// open (MainThreadMenuPump gates Reconcile on this flag), so the change
		// applies the next time the menu opens.
		else if (a_key == "pauseMenuEntry" && a_value.is_boolean()) {
			_config.pauseMenuEntry = a_value.get<bool>();
			PauseMenuEntry::SetEnabled(_config.pauseMenuEntry);
			REX::DEBUG("Runtime: setting osfui.pauseMenuEntry -> {} (applies the next time the pause menu opens)", _config.pauseMenuEntry);
		}
		// Vanilla key-conflict data (MCM-owned). Lazy build / clear.
		else if (a_key == "vanillaKeyConflicts" && a_value.is_boolean()) {
			_config.vanillaKeyConflicts = a_value.get<bool>();
			ApplyVanillaKeyConflicts(_config.vanillaKeyConflicts);
		}
		else if (a_key == "renderStats" && a_value.is_boolean()) {
			_renderStatsEnabled = a_value.get<bool>();
			_renderStatsHaveBaseline = false;
			if (_compositor) {
				_compositor->SetRenderStatsEnabled(_renderStatsEnabled);
			}
			if (_renderer) {
				for (const auto& manifest : _views.All()) {
					if (_menus.IsRegistered(manifest.id)) {
						_renderer->SetRenderStats(manifest.id, _renderStatsEnabled);
					}
				}
			}
			REX::DEBUG("Runtime: setting osfui.renderStats -> {} for all views",
				_renderStatsEnabled);
		}
		else if (a_key == "debugMode" && a_value.is_boolean()) {
			_config.debugMode = a_value.get<bool>();
			BroadcastViewsData();  // debugOnly views appear/leave the mod menu live
			REX::DEBUG("Runtime: setting osfui.debugMode -> {} (developer views {} in the mod menu)",
				_config.debugMode, _config.debugMode ? "shown" : "hidden");
		}
		// Diagnostic-reporter kill switch. Gates manual System Health reports
		// immediately; the host's post-crash prompt reads the endpoint from its
		// spawn arguments, so that half applies on the next launch (Initialize
		// peeks this persisted value before spawning the host).
		else if (a_key == "bugReporting" && a_value.is_boolean()) {
			_config.bugReporting = a_value.get<bool>();
			REX::DEBUG("Runtime: setting osfui.bugReporting -> {}", _config.bugReporting);
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

	void Runtime::RefreshLocalizedData()
	{
		PauseMenuEntry::Configure(
			_localization.Resolve("osfui", "chrome.pauseMenuEntry", _config.pauseMenuEntryLabel),
			_config.pauseMenuEntryView);
		if (_settings) {
			_settings->Store().InvalidateLocalizedData();
			// Rebuild authored game labels under the new locale.
			if (_config.vanillaKeyConflicts) {
				_vanillaKeysApplied = false;
				ApplyVanillaKeyConflicts(true);
			} else {
				_settings->BroadcastData();
			}
		}
		BroadcastViewsData();
		PublishPlatformState("i18n");
	}

	void Runtime::ApplyVanillaKeyConflicts(bool a_enabled)
	{
		if (!_settings || a_enabled == _vanillaKeysApplied) {
			return;  // no store, or already in the requested state
		}
		_vanillaKeysApplied = a_enabled;
		auto& store = _settings->Store();
		if (!a_enabled) {
			store.SetVanillaKeys({});
			REX::DEBUG("Runtime: vanilla key-conflict data disabled");
		} else {
			// The game's own bindings join the conflict grouping as "@game"
			// pseudo-entries (mcm-design.md §9; no engine RE). Defaults come from
			// the curated shipped table — the engine bakes its defaults into the
			// executable and no controlmap ships in the archives. The controlmap
			// text files the engine honors overlay it (mod-provided Data override,
			// then the user's in-game remaps), then the user's additive
			// vanillakeys.user.json: fixes survive updates while untouched rows
			// keep upstream corrections.
			VanillaKeys vanilla;
			if (vanilla.LoadDefaults(Paths::DataDir() / "vanillakeys.json", ResolveKeyName)) {
				const auto scanToVk = [](std::uint32_t a_sc) { return Platform::DirectInputScanToVk(a_sc); };
				// DataDir = <Data>/SFSE/Plugins/OSFUI; under MO2 the module
				// path is virtualized, so this resolves through the VFS too.
				const auto gameData = Paths::DataDir().parent_path().parent_path().parent_path();
				vanilla.OverlayControlMap(gameData / "Interface" / "Controls" / "PC" / "ControlMap.txt", scanToVk);
				const auto docs = Platform::GetDocumentsPath();
				if (!docs.empty()) {
					vanilla.OverlayControlMap(docs / "My Games" / "Starfield" / "ControlMap_Custom.txt", scanToVk);
					vanilla.OverlayUserFile(docs / "My Games" / "Starfield" / "OSFUI" / "vanillakeys.user.json", ResolveKeyName);
				}
				std::vector<SettingsStore::VanillaKey> keys;
				for (const auto& b : vanilla.Bindings()) {
					if (b.vk != 0) {
						// Name resolved after the overlays: a rebound event
						// displays its live key, not the curated default's.
						const auto label = _localization.Resolve("osfui", "gameBindings." + b.event + ".label", b.label);
						const auto owner = _localization.Resolve("osfui", "gameBindings.owner", "Starfield");
						keys.push_back({ b.event, owner + " (" + label + ")", b.vk, KeyName(b.vk) });
					}
				}
				store.SetVanillaKeys(std::move(keys));
			}
		}
		// The conflict annotations live inside the settings document, so re-sync
		// any open view (no-op with no subscribers, e.g. at boot).
		_settings->BroadcastData();
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
		// Keep cursor speed consistent across resolutions: ~1920 counts sweep the
		// view width regardless of view size.
		_cursorScale.store((std::max)(1.0f, static_cast<float>(viewWidth) / 1920.0f));
		_renderer->Resize(viewWidth, viewHeight);
		REX::DEBUG("Runtime: output {}x{} -> view resized to {}x{} (aspect-correct)",
			a_width, a_height, viewWidth, viewHeight);
	}

	void Runtime::SubmitFrameIfVisible()
	{
		if (!_initialized || !IsVisible() || !_renderer || !_compositor) {
			return;
		}
		if (const auto frame = _renderer->Render()) {
			if (!_revealPending) {
				_lastSubmittedFrame = frame->frameIndex;
				_compositor->Submit(*frame);
				return;
			}
			if (frame->frameIndex != _lastSubmittedFrame) {
				_lastSubmittedFrame = frame->frameIndex;
				_compositor->Submit(*frame);  // also starts lazy seam setup
				_revealFrameReady = true;
			}
			// Hold reasons, checked in order; the reveal completes only when none
			// applies. The deadline below runs only for a tick that is still
			// holding, so a frame that becomes presentable after a long stall
			// (alt-tab, load hitch) completes the reveal instead of tripping the
			// timeout it already satisfied.
			bool holding = false;
			if (!_revealFrameReady) {
				// No frame from this exact presentation has reached the game
				// yet. The host stamps frames only after the (re)shown view is
				// presentable, so keep the compositor hidden.
				holding = true;
			} else if (!_compositor->IsOutputSizeKnown()) {
				// The UI seam has not observed the output size yet. Keep the first
				// manifest-sized texture hidden while that callback arrives.
				holding = true;
			} else if (frame->width != _viewWidth.load() ||
					   frame->height != _viewHeight.load()) {
				// The output callback requested a resize, but the host has not
				// painted the correctly sized replacement yet.
				holding = true;
			}
			if (!holding) {
				_revealPending = false;
				_revealFrameReady = false;
				_revealHeldSeconds = 0.0;
				_revealLastPolledAt = {};
				_compositor->SetVisible(true);  // the cached frame is fresh and output-sized
				return;
			}
		} else if (!_revealPending) {
			return;
		}
		// Reveal still held this tick: charge the held-time budget. The per-tick
		// delta is clamped because Tick stalls entirely while the game is
		// unfocused or hitching — wall-clock time spent stalled is not time the
		// renderer failed to produce a frame in.
		const auto now = std::chrono::steady_clock::now();
		if (_revealLastPolledAt != std::chrono::steady_clock::time_point{}) {
			_revealHeldSeconds += (std::min)(std::chrono::duration<double>(
				now - _revealLastPolledAt).count(), 0.25);
		}
		_revealLastPolledAt = now;
		if (_revealHeldSeconds >= kRevealTimeoutSeconds) {
			const auto active = _menus.ActiveMenu().value_or("<none>");
			REX::ERROR("Runtime: overlay reveal for '{}' produced no presentable frame in {:.1f}s "
					   "of live run time — closing it and releasing input/pause state",
				active, _revealHeldSeconds);
			CancelPendingOpen();
			_menus.CloseAll();
			ApplyMenuPolicy();
			// The timeout fires after this tick's normal policy reconciliation.
			// Release every engine-owned edge now instead of trapping input/pause
			// until another main-thread task happens to run.
			if (_config.focusMenu) {
				ReconcileFocusMenu();
			}
			ReconcileControlLayer();
			ReconcileSimPause();
			FreeCursor::Apply(false);
		}
	}

	void Runtime::UpdateRenderDiagnostics()
	{
		if (!_renderStatsEnabled || !IsVisible() || !_renderer || !_compositor) {
			_renderStatsHaveBaseline = false;
			return;
		}

		const auto current = _compositor->GetRenderStats();
		if (!_renderStatsHaveBaseline) {
			_renderStatsBaseline = current;
			_renderStatsLastSampleAt = _uptime;
			_renderStatsHaveBaseline = true;
			return;
		}
		const auto elapsed = _uptime - _renderStatsLastSampleAt;
		if (elapsed < 2.0) return;

		const auto delta = [](const std::uint64_t a_now, const std::uint64_t a_before) {
			return a_now >= a_before ? a_now - a_before : a_now;
		};
		const auto draws = delta(current.draws, _renderStatsBaseline.draws);
		const auto fresh = delta(current.freshFrames, _renderStatsBaseline.freshFrames);
		const auto reused = delta(current.reusedDraws, _renderStatsBaseline.reusedDraws);
		const auto submits = delta(current.submits, _renderStatsBaseline.submits);
		const auto latencyMs = delta(current.sourceToDrawMsTotal,
			_renderStatsBaseline.sourceToDrawMsTotal);
		const auto latencySamples = delta(current.sourceToDrawSamples,
			_renderStatsBaseline.sourceToDrawSamples);
		const auto recordUs = delta(current.recordCpuUsTotal,
			_renderStatsBaseline.recordCpuUsTotal);
		const auto recordSamples = delta(current.recordCpuSamples,
			_renderStatsBaseline.recordCpuSamples);

		const RenderStatsSample sample{
			.drawFps = static_cast<double>(draws) / elapsed,
			.freshFps = static_cast<double>(fresh) / elapsed,
			.submitFps = static_cast<double>(submits) / elapsed,
			.sourceToDrawMs = latencySamples ?
				static_cast<double>(latencyMs) / static_cast<double>(latencySamples) : 0.0,
			.recordCpuMs = recordSamples ?
				static_cast<double>(recordUs) / (1000.0 * static_cast<double>(recordSamples)) : 0.0,
			.reusedDraws = reused,
		};
		_renderer->SetRenderStatsSample(sample);
		REX::INFO(
			"Render diagnostics ({:.2f}s): fresh view {:.1f} fps, overlay passes {:.1f}/s "
			"({} reused), frame submit {:.1f} fps; source-to-draw {:.2f} ms, "
			"record CPU {:.3f} ms; path={}, FG={}",
			elapsed, sample.freshFps, sample.drawFps, reused, sample.submitFps,
			sample.sourceToDrawMs, sample.recordCpuMs,
			current.seamActive ? "UI seam" : "unavailable", current.frameGeneration ? "on" : "off");

		_renderStatsBaseline = current;
		_renderStatsLastSampleAt = _uptime;
	}

	std::unique_ptr<IWebRenderer> Runtime::CreateRenderer() const
	{
		if (_config.renderer == "null") {
			return std::make_unique<NullWebRenderer>();
		}
		if (_config.renderer == "webview2") {
#if defined(OSFUI_WITH_WEBVIEW2)
			// Out-of-process host backend: the only WebView2 variant that works
			// under Mod Organizer 2 without the manual executable-blacklist
			// workaround (USVFS injection crashes in-process-spawned browsers).
			// A missing Evergreen runtime is reported by the host over the hello
			// handshake, not probed here — see WebView2HostWebRenderer.
			return std::make_unique<WebView2HostWebRenderer>();
#else
			REX::WARN("Runtime: renderer 'webview2' requested but this build was compiled without "
					  "with_webview2; using null renderer");
			return std::make_unique<NullWebRenderer>();
#endif
		}
		REX::WARN("Runtime: unknown renderer '{}'; using null renderer", _config.renderer);
		return std::make_unique<NullWebRenderer>();
	}

	std::unique_ptr<ICompositor> Runtime::CreateCompositor() const
	{
		if (_config.compositor == "d3d12") {
			// Samples the host's shared texture in the engine UI pass; device
			// and queue discovery remain lazy (see composite/EngineD3D12.h).
			return std::make_unique<D3D12Compositor>();
		}
		if (_config.compositor != "null") {
			REX::WARN("Runtime: unknown compositor '{}'; using null compositor", _config.compositor);
		}
		return std::make_unique<NullCompositor>();
	}

}
