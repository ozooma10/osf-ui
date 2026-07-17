#pragma once

// Optional Ultralight backend. Only compiled when the xmake option
// `with_ultralight` is enabled (defines OSFUI_WITH_ULTRALIGHT=1 and requires
// the ULTRALIGHT_SDK_DIR environment variable). The SDK is proprietary and is
// never vendored into this repository.

#if defined(OSFUI_WITH_ULTRALIGHT)

	#include "render/IWebRenderer.h"

namespace OSFUI
{
	// Offscreen Ultralight-based HTML renderer.
	//
	// Threading model: ALL Ultralight/WebCore calls happen on one dedicated
	// worker thread owned by this class. The game-facing IWebRenderer methods
	// only touch lock-protected queues and a double-buffered frame copy. This
	// is required because WebKit is thread-affine while SFSE permanent tasks
	// arrive on varying OS threads (proven in-game 2026-06-12), and it keeps
	// per-tick cost on the game thread near zero. The worker touches only
	// Ultralight state and this class's buffers — never game state.
	//
	// All SDK types stay behind the Impl so non-Ultralight translation units
	// never see SDK headers.
	class UltralightWebRenderer final : public IWebRenderer
	{
	public:
		// MUST be called (and succeed) before constructing this class. The
		// SDK DLLs are delay-loaded, and merely constructing the renderer
		// touches imported symbols (the Impl's SDK base-class ctors), which
		// fires delay-load resolution. This preloads the DLLs from
		// <dataDir>/ultralight/bin so that resolution finds them already in
		// the process. Touches no SDK symbols itself; logs and returns false
		// if anything is missing.
		[[nodiscard]] static bool PreloadRuntime(const std::filesystem::path& a_dataDir);

		UltralightWebRenderer();
		~UltralightWebRenderer() override;

		bool Initialize(const RendererConfig& a_config) override;
		void Shutdown() override;
		void LoadView(const ViewManifest& a_manifest) override;
		void SetActiveView(std::string_view a_id) override;
		void Resize(std::uint32_t a_width, std::uint32_t a_height) override;
		void Update(double a_deltaSeconds) override;
		std::optional<FrameBufferView> Render() override;
		void SendMessageToWeb(std::string_view a_viewId, std::string_view a_json) override;
		void SetWebMessageHandler(WebMessageHandler a_handler) override;
		void InjectKeyEvent(std::uint32_t a_vkCode, bool a_down) override;
		void InjectCharEvent(std::uint32_t a_codepoint) override;
		void InjectMouseMove(int a_x, int a_y) override;
		void InjectMouseButton(int a_x, int a_y, int a_button, bool a_down) override;
		void InjectMouseWheel(int a_x, int a_y, int a_wheelDelta) override;

		// Per-view JS interaction + lifecycle hooks (see IWebRenderer.h).
		void SetDomReadyHandler(DomReadyHandler a_handler) override;
		void SetLoadHandler(LoadHandler a_handler) override;
		void SetCursorChangeHandler(CursorChangeHandler a_handler) override;
		void EvaluateScript(std::string_view a_viewId, std::string_view a_js, ScriptResultHandler a_onResult) override;
		void CallJsFunction(std::string_view a_viewId, std::string_view a_fnName, std::string_view a_arg) override;
		void RegisterJsFunction(std::string_view a_viewId, std::string_view a_name, JsListenerHandler a_callback) override;
		void SetConsoleHandler(std::string_view a_viewId, ConsoleHandler a_handler) override;
		void SetViewHidden(std::string_view a_viewId, bool a_hidden) override;
		void SetViewOrder(std::string_view a_viewId, int a_order) override;
		void SetScrollPixelSize(std::string_view a_viewId, int a_pixels) override;
		void DestroyView(std::string_view a_viewId) override;

		[[nodiscard]] std::string_view Name() const override { return "ultralight"; }

	private:
		struct Impl;
		std::unique_ptr<Impl> _impl;
	};
}

#endif  // OSFUI_WITH_ULTRALIGHT
