#pragma once

#include "runtime/ViewManifest.h"

namespace OSFUI
{
	struct RendererConfig
	{
		std::uint32_t width{ kDefaultViewWidth };
		std::uint32_t height{ kDefaultViewHeight };
		bool          devMode{ false };

		// Plugin data root (Paths::DataDir()). Backends resolve packaged assets
		// such as bin/osfui_webview2_host.exe under here, keeping render/
		// decoupled from core/Paths.
		std::filesystem::path dataDir;
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

	// Region of a frame that changed relative to the frame delivered just
	// before it at the same dimensions; pixels outside the rect are identical.
	// An empty rect means "unknown — treat the whole frame as changed", so
	// producers that don't track dirty regions stay correct by default.
	struct DirtyRect
	{
		std::uint32_t x{ 0 };
		std::uint32_t y{ 0 };
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };

		[[nodiscard]] bool Empty() const { return width == 0 || height == 0; }

		[[nodiscard]] static DirtyRect Full(std::uint32_t a_w, std::uint32_t a_h)
		{
			return DirtyRect{ 0, 0, a_w, a_h };
		}

		// Bounding box of both rects; an empty side yields the other unchanged.
		[[nodiscard]] static DirtyRect Union(const DirtyRect& a_a, const DirtyRect& a_b)
		{
			if (a_a.Empty()) {
				return a_b;
			}
			if (a_b.Empty()) {
				return a_a;
			}
			const auto x0 = (std::min)(a_a.x, a_b.x);
			const auto y0 = (std::min)(a_a.y, a_b.y);
			const auto x1 = (std::max)(a_a.x + a_a.width, a_b.x + a_b.width);
			const auto y1 = (std::max)(a_a.y + a_a.height, a_b.y + a_b.height);
			return DirtyRect{ x0, y0, x1 - x0, y1 - y0 };
		}
	};

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
		// Changed region vs the previous frame this renderer returned at the
		// same dimensions (empty = whole frame). Only meaningful when
		// frameIndex advanced; consumers keeping their own staging copy must
		// union rects across frames they skipped.
		DirtyRect                     dirty{};
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

		// Fires once per view when its DOM becomes ready, on the game thread
		// (drained from Update()). Backs the consumer API's CreateView DOM-ready
		// callback. Set once before LoadView.
		using DomReadyHandler = std::function<void(std::string_view a_viewId)>;
		virtual void SetDomReadyHandler(DomReadyHandler) {}

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
			std::uint32_t /*a_devReloadVk*/, bool /*a_captured*/,
			bool /*a_captureArmed*/, std::uint32_t /*a_captureUpVk*/) {}

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
		// of WHEEL_DELTA (120); positive scrolls up. The backend converts notches
		// to pixels via the per-view scroll step (SetScrollPixelSize).
		virtual void InjectMouseWheel(int /*a_x*/, int /*a_y*/, int /*a_wheelDelta*/) {}

		// Per-view JS interaction primitives. Backends marshal the work onto
		// their own worker thread and deliver callbacks on the game thread
		// (drained from Update()), like SetWebMessageHandler. No-ops by default
		// so non-JS backends (null/mock) still link. a_viewId is the
		// manifest/internal view id.

		// If a_onResult is set it receives the expression result as a string, on
		// the game thread.
		using ScriptResultHandler = std::function<void(std::string a_result)>;
		virtual void EvaluateScript(std::string_view /*a_viewId*/, std::string_view /*a_js*/,
			ScriptResultHandler /*a_onResult*/ = nullptr) {}

		// Call window.<a_fnName>(a_arg) directly, no eval.
		virtual void CallJsFunction(std::string_view /*a_viewId*/, std::string_view /*a_fnName*/,
			std::string_view /*a_arg*/) {}

		// Bind window.<a_name>(str) in the view; a_callback fires on the game
		// thread with the single string argument.
		using JsListenerHandler = std::function<void(std::string a_argument)>;
		virtual void RegisterJsFunction(std::string_view /*a_viewId*/, std::string_view /*a_name*/,
			JsListenerHandler /*a_callback*/) {}

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
		virtual void SetScrollPixelSize(std::string_view /*a_viewId*/, int /*a_pixels*/) {}
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
