#pragma once

#include "render/IWebRenderer.h"

namespace OSFUI
{
#if defined(OSFUI_WITH_WEBVIEW2)
	class WebView2WebRenderer final : public IWebRenderer
	{
	public:
		WebView2WebRenderer();
		~WebView2WebRenderer() override;

		[[nodiscard]] static bool RuntimeAvailable();

		bool Initialize(const RendererConfig& a_config) override;
		void Shutdown() override;
		void LoadView(const ViewManifest& a_manifest) override;
		void SetActiveView(std::string_view a_id) override;
		[[nodiscard]] bool SupportsMultipleViews() const override { return false; }
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
		void DestroyView(std::string_view a_viewId) override;
		[[nodiscard]] std::string_view Name() const override { return "webview2"; }

	private:
		struct Impl;
		std::unique_ptr<Impl> _impl;
	};
#endif
}
