#pragma once

#include "render/IWebRenderer.h"

namespace OSFUI
{
#if defined(OSFUI_WITH_WEBVIEW2)
	// Out-of-process WebView2 backend (renderer id "webview2"). The browser
	// stack lives in osfui_webview2_host.exe, launched OUTSIDE the game's
	// process tree (tools/webview2_shared/Wv2BrokerLaunch) so Mod Organizer
	// 2's USVFS never injects into msedgewebview2.exe — the zero-config fix
	// for the in-process backend's E_UNEXPECTED controller failure under MO2.
	//
	// The plugin is a thin client: one named pipe carries control/input/
	// bridge traffic (tools/webview2_shared/Wv2Protocol.h), and frames arrive
	// as GPU shared textures the D3D12 compositor samples directly (no CPU
	// readback). Keyboard stays the real-focus model: the host parents its
	// browser HWND beneath the game window (window tree != process tree) and
	// framework keys come back over the pipe.
	//
	// Multi-view: the host keeps one composition controller + child visual
	// per view under ONE captured root, so sibling plugin views and the
	// configured layer set all composite through the same shared-texture
	// ring; this client just routes per-view ids over the pipe.
	//
	// This is the only browser backend. The former in-process variant
	// ("webview2-inproc") was removed: it needed a manual MO2 executable
	// blacklist entry to survive USVFS injection, and offered nothing this
	// does not.
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
		void SetNativeKeyboardFocus(bool a_focused) override;
		[[nodiscard]] bool UsesNativeKeyboardFocus() const override { return true; }
		void SetAcceleratorKeys(std::uint32_t a_toggleVk, std::uint32_t a_devReloadVk,
			bool a_captured, bool a_captureArmed, std::uint32_t a_captureUpVk) override;
		void SetSharedRingHandler(SharedRingHandler a_handler) override;
		void InjectKeyEvent(std::uint32_t a_vkCode, bool a_down) override;
		void InjectMouseMove(int a_x, int a_y) override;
		void InjectMouseButton(int a_x, int a_y, int a_button, bool a_down) override;
		void InjectMouseWheel(int a_x, int a_y, int a_wheelDelta) override;
		void EvaluateScript(std::string_view a_viewId, std::string_view a_js,
			ScriptResultHandler a_onResult = nullptr) override;
		void CallJsFunction(std::string_view a_viewId, std::string_view a_fnName,
			std::string_view a_arg) override;
		void RegisterJsFunction(std::string_view a_viewId, std::string_view a_name,
			JsListenerHandler a_callback) override;
		void SetConsoleHandler(std::string_view a_viewId, ConsoleHandler a_handler) override;
		void SetViewHidden(std::string_view a_viewId, bool a_hidden) override;
		void SetViewOrder(std::string_view a_viewId, int a_order) override;
		void DestroyView(std::string_view a_viewId) override;
		[[nodiscard]] std::string_view Name() const override { return "webview2"; }

	private:
		struct Impl;
		std::unique_ptr<Impl> _impl;
	};
#endif
}
