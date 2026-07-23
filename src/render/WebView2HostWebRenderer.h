#pragma once

#include "render/IWebRenderer.h"

namespace OSFUI
{
#if defined(OSFUI_WITH_WEBVIEW2)
	// Out-of-process WebView2 backend (renderer id "webview2"), and the only
	// browser backend. The browser stack lives in osfui_webview2_host.exe,
	// launched outside the game's process tree (Wv2BrokerLaunch) so MO2's
	// USVFS never injects into msedgewebview2.exe — that injection is what
	// made the removed in-process variant fail controller creation with
	// E_UNEXPECTED unless the user added an MO2 blacklist entry by hand.
	//
	// The plugin is a thin client: one named pipe carries control/input/bridge
	// traffic (Wv2Protocol.h), and frames arrive as GPU shared textures the
	// D3D12 compositor samples directly (no CPU readback). Keyboard uses the
	// real-focus model: the host parents its browser HWND beneath the game
	// window (window tree != process tree) and framework keys come back over
	// the pipe.
	//
	// Multi-view: the host keeps one composition controller + child visual per
	// view under a single captured root, so all views composite through the
	// same shared-texture ring; this client just routes per-view ids.
	class WebView2HostWebRenderer final : public IWebRenderer
	{
	public:
		WebView2HostWebRenderer();
		~WebView2HostWebRenderer() override;

		bool Initialize(const RendererConfig& a_config) override;
		void Shutdown() override;
		void LoadView(const ViewManifest& a_manifest) override;
		void SetActiveView(std::string_view a_id) override;
		[[nodiscard]] bool SupportsMultipleViews() const override { return true; }
		void Resize(std::uint32_t a_width, std::uint32_t a_height) override;
		void Update(double a_deltaSeconds) override;
		std::optional<FrameBufferView> Render() override;
		void SendMessageToWeb(std::string_view a_viewId, std::string_view a_json) override;
		void SetWebMessageHandler(WebMessageHandler a_handler) override;
		void SetDomReadyHandler(DomReadyHandler a_handler) override;
		void SetLoadHandler(LoadHandler a_handler) override;
		void SetCursorChangeHandler(CursorChangeHandler a_handler) override;
		void SetNativeAcceleratorHandler(NativeAcceleratorHandler a_handler) override;
		void SetNativeFocus(bool a_focused) override;
		[[nodiscard]] bool UsesNativeKeyboardFocus() const override { return true; }
		void SetAcceleratorKeys(std::uint32_t a_toggleVk, std::uint32_t a_devReloadVk,
			bool a_captured, bool a_captureArmed, std::uint32_t a_captureUpVk) override;
		void SetSharedRingHandler(SharedRingHandler a_handler) override;
		void InjectKeyEvent(std::uint32_t a_vkCode, bool a_down) override;
		void InjectMouseMove(int a_x, int a_y) override;
		void InjectMouseButton(int a_x, int a_y, int a_button, bool a_down) override;
		void InjectMouseWheel(int a_x, int a_y, int a_wheelDelta) override;
		void InjectPhysicalMouseWheel(int a_x, int a_y, int a_wheelDelta) override;
		void EvaluateScript(std::string_view a_viewId, std::string_view a_js,
			ScriptResultHandler a_onResult = nullptr) override;
		void CallJsFunction(std::string_view a_viewId, std::string_view a_fnName,
			std::string_view a_arg) override;
		void RegisterJsFunction(std::string_view a_viewId, std::string_view a_name,
			JsListenerHandler a_callback) override;
		void SetConsoleHandler(std::string_view a_viewId, ConsoleHandler a_handler) override;
		void SetViewHidden(std::string_view a_viewId, bool a_hidden) override;
		void PrewarmView(std::string_view a_viewId) override;
		void SetViewOrder(std::string_view a_viewId, int a_order) override;
		void SetRenderStats(std::string_view a_viewId, bool a_enabled) override;
		void SetRenderStatsSample(const RenderStatsSample& a_sample) override;
		void DestroyView(std::string_view a_viewId) override;
		[[nodiscard]] std::string_view Name() const override { return "webview2"; }

	private:
		struct Impl;
		std::unique_ptr<Impl> _impl;
	};
#endif
}
