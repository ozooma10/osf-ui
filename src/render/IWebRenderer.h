#pragma once

#include "runtime/ViewManifest.h"

namespace OSFUI
{
	struct RendererConfig
	{
		std::uint32_t width{ kDefaultViewWidth };
		std::uint32_t height{ kDefaultViewHeight };
		bool          devMode{ false };
		// Host-owned HTTPS destination for the consented crash prompt. Empty
		// disables it. Only the primary overlay renderer receives this value.
		std::string   reportEndpoint;
		// Actual installed plugin root; the helper runs from a LocalAppData
		// mirror, so it cannot derive this path for crash-log redaction.
		std::filesystem::path reportPluginRoot;

		// Plugin data root (Paths::DataDir()). Backends resolve packaged assets
		// such as bin/osfui_webview2_host.exe under here, keeping render/
		// decoupled from core/Paths.
		std::filesystem::path dataDir;

#if defined(OSFUI_WITH_WORLD_SURFACES)
		// Identity of this renderer's out-of-process browser host. Empty is the
		// primary overlay instance; a non-empty tag (e.g. "world") runs a fully
		// independent second host with its own pipe, single-instance lock,
		// WebView2 user-data folder, views mirror, and host log file. Keep it a
		// short lowercase file-name-safe tag.
		std::string instanceName;
#endif
	};

	enum class PixelFormat
	{
		kRGBA8,
		kBGRA8,
	};

	// Cursor shape a page wants (CSS `cursor`), mirrored onto the OS pointer
	// while the overlay captures input (input/HardwareCursor). Web cursors with
	// no stock Win32 equivalent collapse to the nearest listed one (kArrow when
	// nothing fits). kNone means `cursor: none`.
	enum class CursorShape
	{
		kArrow,
		kCross,
		kHand,
		kIBeam,
		kWait,
		kHelp,
		kNotAllowed,
		kSizeWE,
		kSizeNS,
		kSizeNESW,
		kSizeNWSE,
		kSizeAll,
		kNone,
	};

	// ICoreWebView2CompositionController::get_SystemCursorId returns the Win32
	// OCR_* integer IDs. Keep this platform-neutral so the translation can be
	// tested without loading WebView2 or the game.
	[[nodiscard]] constexpr CursorShape CursorShapeFromSystemCursorId(std::uint32_t a_id) noexcept
	{
		switch (a_id) {
		case 0:     return CursorShape::kNone;
		case 32513: return CursorShape::kIBeam;       // OCR_IBEAM
		case 32514: return CursorShape::kWait;        // OCR_WAIT
		case 32515: return CursorShape::kCross;       // OCR_CROSS
		case 32642: return CursorShape::kSizeNWSE;    // OCR_SIZENWSE
		case 32643: return CursorShape::kSizeNESW;    // OCR_SIZENESW
		case 32644: return CursorShape::kSizeAll;     // OCR_SIZEALL
		case 32645: return CursorShape::kSizeWE;      // OCR_SIZEWE
		case 32646: return CursorShape::kSizeNS;      // OCR_SIZENS
		case 32648: return CursorShape::kNotAllowed;  // OCR_NO
		case 32649: return CursorShape::kHand;        // OCR_HAND
		case 32651: return CursorShape::kHelp;        // OCR_HELP
		default:    return CursorShape::kArrow;
		}
	}

	// Non-owning view of one CPU-side frame produced by a renderer.
	//
	// The pixel data is owned by the renderer and valid only until the next
	// Render(), Resize(), or Shutdown() on it. Consumers (compositors) must
	// copy or upload before returning control; never store a FrameBufferView.
	struct FrameBufferView
	{
		std::span<const std::uint8_t> pixels;  // tightly packed rows unless strideBytes says otherwise
		std::uint32_t                 width{ 0 };
		std::uint32_t                 height{ 0 };
		std::uint32_t                 strideBytes{ 0 };  // bytes per row
		PixelFormat                   format{ PixelFormat::kRGBA8 };
		std::uint64_t                 frameIndex{ 0 };
		// GPU transport (out-of-process WebView2 host): when >= 0, `pixels` is
		// empty and the frame lives in slot `sharedSlot` of the shared-texture
		// ring the renderer announced via the SharedRingHandler. frameIndex
		// doubles as the produce-fence value for that slot. CPU-only
		// compositors must ignore such frames.
		std::int32_t                  sharedSlot{ -1 };
		// GetTickCount64 timestamp taken when the source frame entered the
		// renderer transport. Windows uptime is system-wide, so the compositor
		// can measure source-to-draw latency even when capture happens in the
		// out-of-process WebView2 host. Zero means unavailable.
		std::uint64_t                 sourceTimeMs{ 0 };
	};

	// One interval of game-side render diagnostics. Cumulative compositor
	// counters are reduced by Runtime before this reaches the renderer, which
	// may display it in its host-owned diagnostics UI.
	struct RenderStatsSample
	{
		double presentFps{ 0.0 };
		double drawFps{ 0.0 };
		double freshFps{ 0.0 };
		double submitFps{ 0.0 };
		double sourceToDrawMs{ 0.0 };
		double recordCpuMs{ 0.0 };
		std::uint64_t reusedDraws{ 0 };
		std::uint64_t busyWaits{ 0 };
		std::uint64_t droppedBusy{ 0 };
		std::uint64_t skippedConcurrent{ 0 };
		bool seamMode{ false };
		bool frameGeneration{ false };
	};

	// Cross-process shared-texture ring produced by the out-of-process WebView2
	// host, announced by the renderer on the game thread. Handles are already
	// valid in this process (duplicated by the producer) and owned by the
	// consumer once delivered: open with ID3D12Device::OpenSharedHandle, then
	// CloseHandle. Sync is a produce fence (wait for value == frameIndex before
	// sampling a slot) and a consume fence (signal frameIndex after the GPU read
	// completes, so the producer may rewrite the slot). A new announcement
	// (higher generation) invalidates every prior slot.
	struct SharedRingDesc
	{
		// Capacity only — the producer announces the actual ring depth per
		// generation (slotCount); entries past it stay null. Keeps the depth a
		// producer-side tuning knob instead of a cross-binary constant.
		static constexpr std::size_t kMaxSlots = 8;

		void*         slotHandles[kMaxSlots]{};
		std::uint32_t slotCount{ 0 };
		void*         produceFence{ nullptr };
		void*         consumeFence{ nullptr };
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		// Adapter that created the resources, copied from the host's D3D11
		// device. Used only for diagnostics when D3D12 rejects a handle.
		std::uint32_t adapterLuidLow{ 0 };
		std::uint32_t adapterLuidHigh{ 0 };
		std::uint64_t generation{ 0 };
	};

	// Renderer backend interface. Backends render web (or fake) content into a
	// CPU buffer; they know nothing about the game, D3D12, or hooks.
	class IWebRenderer
	{
	public:
		virtual ~IWebRenderer() = default;

		virtual bool Initialize(const RendererConfig& a_config) = 0;
		virtual void Shutdown() = 0;

		// Loads (or replaces) a view by its manifest id. Multi-view backends keep
		// previously-loaded views so several composite at once; the first loaded
		// view becomes active by default. Use SetActiveView to change that.
		virtual void LoadView(const ViewManifest& a_manifest) = 0;

        // Development-only loose-file support. This may run on the dev worker,
        // so implementations must synchronize any mutable filesystem state.
        // Direct-path and non-browser backends need no work. False asks the
        // caller to retry after an editor or antivirus releases a file.
        virtual bool RefreshViewFiles(std::string_view /*a_viewId*/) { return true; }

		// Selects which loaded view receives input (and, today, the bridge).
		// No-op if the id is not loaded, or for single-view backends.
		virtual void SetActiveView(std::string_view /*a_id*/) {}
		[[nodiscard]] virtual bool SupportsMultipleViews() const { return true; }

		// Resizes the view surface(s). Multi-view backends resize every hosted
		// view to the same output size so their frames composite 1:1.
		virtual void Resize(std::uint32_t a_width, std::uint32_t a_height) = 0;
		virtual void Update(double a_deltaSeconds) = 0;

		// Returns the current frame, or std::nullopt if there is nothing to
		// present. See FrameBufferView for the lifetime contract.
		virtual std::optional<FrameBufferView> Render() = 0;

		// Delivers a JSON message to one view (native -> web); a_viewId is the
		// target's manifest id. Backends without a JS engine may log and drop it;
		// single-view backends may ignore the id.
		virtual void SendMessageToWeb(std::string_view a_viewId, std::string_view a_json) = 0;

		// Receives JSON messages (web -> native), tagged with the source view id
		// so responses route back. Invoked from Update() on the calling (game)
		// thread, never from a renderer-internal thread. Set before LoadView.
		using WebMessageHandler = std::function<void(std::string_view a_viewId, std::string_view a_json)>;
		virtual void SetWebMessageHandler(WebMessageHandler) {}

		// A main-frame load reaching a terminal state, on the game thread
		// (drained from Update()). `failed` false means success and is followed
		// by DOM-ready; true means failure, with description/errorDomain/
		// errorCode carrying the backend's diagnostics. A failed load does not
		// fire DOM-ready, so this is the only signal that a view did not come
		// up. Set once before LoadView.
		struct LoadEvent
		{
			std::string_view viewId;
			bool             failed{ false };
			std::string_view url;
			std::string_view description;   // failed only
			std::string_view errorDomain;   // failed only
			int              errorCode{ 0 };  // failed only
		};
		using LoadHandler = std::function<void(const LoadEvent& a_event)>;
		virtual void SetLoadHandler(LoadHandler) {}

		// A terminal backend failure that leaves no drawable frontend. Unlike a
		// per-view LoadEvent, this invalidates the renderer for the rest of the
		// session. The runtime must immediately release any modal menu policy;
		// otherwise an invisible overlay can keep gameplay input and pause state.
		// Fired on the game thread from Update(); set once before LoadView.
		struct FailureEvent
		{
			std::string_view stage;
			std::string_view viewId;
			std::string_view description;
			std::uint32_t    errorCode{ 0 };
		};
		using FailureHandler = std::function<void(const FailureEvent& a_event)>;
		virtual void SetFailureHandler(FailureHandler) {}

		// Backend health worth surfacing in the Mods surface's System Health pane
		// (bridge protocol 1.4). Only DEGRADED-BUT-ALIVE conditions belong here —
		// a reduced shared-texture ring, focus stranded in the backend's own
		// child window. A backend that cannot render at all reports through the
		// log and the launch dialog instead, because there is no frontend left
		// to draw a card in. `code` is a stable machine string the built-in
		// frontend maps to player-facing copy; `detail` is short technical text
		// shown only under the card's disclosure and must carry no absolute
		// paths. Fired on the game thread, drained from Update(), and both edges
		// are reported: `active` false means the condition cleared. Set once
		// before LoadView.
		struct HealthEvent
		{
			std::string_view code;
			bool             active{ true };
			std::string_view detail;
		};
		using HealthHandler = std::function<void(const HealthEvent& a_event)>;
		virtual void SetHealthHandler(HealthHandler) {}

		// Fires when the active (input) view's requested cursor changes, so the
		// host can switch the real OS pointer (hover feedback, text I-beam).
		// WARNING: unlike the other handlers, this may be invoked from a
		// renderer-internal thread — the handler must be cheap and thread-safe.
		// The in-tree consumer is a single atomic store; the window hook applies
		// the shape on the next mouse message. Set once before LoadView.
		using CursorChangeHandler = std::function<void(CursorShape a_shape)>;
		virtual void SetCursorChangeHandler(CursorChangeHandler) {}

		// Backends such as WebView2 receive keyboard/IME through a real focused
		// native child window rather than InjectCharEvent. The runtime uses this
		// seam for the whole interactive-menu session; HUD-only and closed states
		// revoke it so the game remains the foreground owner. While the WebView
		// holds focus, the backend's
		// AcceleratorKeyPressed hook delegates framework-owned keys (toggle, Esc,
		// key capture) back to the runtime; the callback returns true when
		// Chromium must mark that accelerator handled.
		using NativeAcceleratorHandler = std::function<bool(std::uint32_t a_vkCode, bool a_down)>;
		virtual void SetNativeAcceleratorHandler(NativeAcceleratorHandler) {}
		virtual void SetNativeFocus(bool /*a_focused*/) {}
		[[nodiscard]] virtual bool UsesNativeKeyboardFocus() const { return false; }

		// Out-of-process backends decide synchronously, in the host process,
		// whether a key is framework-owned and must be withheld from the page,
		// so the host needs a mirror of the runtime's accelerator state. The
		// runtime pushes it every tick; backends diff and forward on change.
		// No-op for in-process backends, which call the accelerator handler
		// directly instead.
		virtual void SetAcceleratorKeys(std::uint32_t /*a_toggleVk*/,
			bool /*a_captured*/,
			bool /*a_captureArmed*/, std::uint32_t /*a_captureUpVk*/) {}

		// The game window received a player-initiated close (WM_CLOSE, an
		// SC_CLOSE system command, or session end). Starfield's forced teardown
		// routinely exits with a non-zero process status, so out-of-process
		// backends forward this to their host, which then treats the exit that
		// follows as intentional instead of offering its crash-report prompt.
		// Called on the window-message thread; must be thread-safe.
		virtual void NotifyPlayerCloseRequest() {}

		// Announces (or replaces) the renderer's GPU shared-texture ring, on the
		// game thread (drained from Update()). The runtime forwards it to the
		// compositor, which owns the handles from then on — see SharedRingDesc.
		// Only fired by backends that produce sharedSlot frames.
		using SharedRingHandler = std::function<void(const SharedRingDesc&)>;
		virtual void SetSharedRingHandler(SharedRingHandler) {}

		// Delivers one keyboard transition into the web view. a_vkCode is a
		// Windows virtual-key code (the space Starfield ButtonEvents carry).
		// Thread-safe to call from the input thread; backends dispatch onto
		// their own thread.
		virtual void InjectKeyEvent(std::uint32_t /*a_vkCode*/, bool /*a_down*/) {}

		// Delivers one text character into the web view: a finished Unicode
		// scalar from the OS char stream (WM_CHAR/WM_UNICHAR), already resolved
		// for the active layout, dead keys, and AltGr. Queued after the matching
		// key's RawKeyDown. Thread-safe.
		virtual void InjectCharEvent(std::uint32_t /*a_codepoint*/) {}

		// Mouse input in view pixel coordinates (0..width, 0..height). Move
		// reports an absolute position — the caller maintains a virtual cursor,
		// since the OS cursor is hidden in gameplay. Button uses MouseButton
		// order (0=left, 1=right, 2=middle). Thread-safe.
		virtual void InjectMouseMove(int /*a_x*/, int /*a_y*/) {}
		virtual void InjectMouseButton(int /*a_x*/, int /*a_y*/, int /*a_button*/, bool /*a_down*/) {}
		// Mouse wheel at a view-space position. a_wheelDelta is a signed multiple
		// of WHEEL_DELTA (120); positive scrolls up. The backend forwards the raw
		// delta to the host's WebView2 WHEEL input, which performs the scroll.
		virtual void InjectMouseWheel(int /*a_x*/, int /*a_y*/, int /*a_wheelDelta*/) {}
		// Physical mouse-wheel fallback from the game window. The out-of-process
		// WebView2 backend normally captures this directly once it owns menu focus;
		// keeping a distinct path lets it ignore the duplicate game-side raw packet
		// while still using it if host-side raw-input registration failed.
		virtual void InjectPhysicalMouseWheel(int a_x, int a_y, int a_wheelDelta)
		{
			InjectMouseWheel(a_x, a_y, a_wheelDelta);
		}

		// Open the browser's native developer tools for one view. Production
		// backends may ignore this; the runtime exposes it only in devMode.
		virtual void OpenDevTools(std::string_view /*a_viewId*/) {}

		// NOTE: there is deliberately no "evaluate this script text" primitive.
		// EvaluateScript / CallJsFunction / RegisterJsFunction existed here and
		// were never called by anything; they were removed rather than left as a
		// standing way to run caller-supplied script inside a privileged view.
		// Native->page communication goes through the typed bridge
		// (SetWebMessageHandler / PostWebMessage), whose payloads are JSON-encoded
		// rather than concatenated into source text.

		// Receive console.* from a view on the game thread; a_level is
		// 0=log,1=warning,2=error,3=debug,4=info. nullptr unsubscribes.
		using ConsoleHandler = std::function<void(int a_level, std::string a_message)>;
		virtual void SetConsoleHandler(std::string_view /*a_viewId*/, ConsoleHandler /*a_handler*/) {}

		// Per-view state mutated at runtime. Honored by multi-view backends in
		// the compositing/scroll path; others ignore.
		virtual void SetViewHidden(std::string_view /*a_viewId*/, bool /*a_hidden*/) {}
		// Prime a hidden view's first Chromium paint, then suspend it again. This
		// keeps an on-demand platform surface cheap while removing the cold
		// renderer/controller path from its first reveal.
		virtual void PrewarmView(std::string_view /*a_viewId*/) {}
		virtual void SetViewOrder(std::string_view /*a_viewId*/, int /*a_order*/) {}
		// Host-owned diagnostics drawn inside one view. The overlay must not
		// require cooperation from (or changes to) the authored page.
		virtual void SetRenderStats(std::string_view /*a_viewId*/, bool /*a_enabled*/) {}
		// Game-side half of the diagnostics sample (present/compositor cadence).
		// Backends without a host-owned panel ignore it.
		virtual void SetRenderStatsSample(const RenderStatsSample& /*a_sample*/) {}

		virtual void DestroyView(std::string_view /*a_viewId*/) {}

		[[nodiscard]] virtual std::string_view Name() const = 0;
	};
}
