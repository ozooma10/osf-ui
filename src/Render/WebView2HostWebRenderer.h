#pragma once

#include "Input/CursorShape.h"
#include "Render/SharedTextureTransport.h"
#include "Views/ViewManifest.h"

namespace OSFUI
{
	struct WebView2HostConfig
	{
		std::uint32_t width{ kDefaultViewWidth };
		std::uint32_t height{ kDefaultViewHeight };
		bool          devMode{ false };
		std::filesystem::path dataDir;
	};

	// Out-of-process WebView2 client using a named pipe and shared-texture ring to avoid MO2 injection.
	class WebView2HostWebRenderer
	{
	public:
		struct LoadEvent
		{
			std::string_view viewId;
			bool             failed{ false };
			std::string_view url;
			std::string_view description;
			std::string_view errorDomain;
			int              errorCode{ 0 };
		};

		struct FailureEvent
		{
			std::string_view stage;
			std::string_view viewId;
			std::string_view description;
			std::uint32_t    errorCode{ 0 };
		};

		struct HealthEvent
		{
			std::string_view code;
			bool             active{ true };
			std::string_view detail;
		};

		using WebMessageHandler = std::function<void(std::string_view a_viewId, std::string_view a_json)>;
		using LoadHandler = std::function<void(const LoadEvent& a_event)>;
		using FailureHandler = std::function<void(const FailureEvent& a_event)>;
		using HealthHandler = std::function<void(const HealthEvent& a_event)>;
		using CursorChangeHandler = std::function<void(CursorShape a_shape)>;
		using NativeAcceleratorHandler =
			std::function<bool(std::uint32_t a_vkCode, std::uint32_t a_scanCode, bool a_down)>;
		using SharedRingHandler = std::function<void(const SharedRingDesc& a_desc)>;
		using ConsoleHandler = std::function<void(int a_level, std::string a_message)>;
		// Update drains game-thread callbacks; cursor and accelerator callbacks run on the transport thread.

		WebView2HostWebRenderer();
		~WebView2HostWebRenderer();

		bool Initialize(const WebView2HostConfig& a_config);
		bool RestartAfterFailure();
		void CreateOrNavigateView(const ViewManifest& a_manifest);
		bool RefreshViewFiles(std::string_view a_viewId);
		void SetInputTargetView(std::string_view a_id);
		void Resize(std::uint32_t a_width, std::uint32_t a_height);
		void Update(double a_deltaSeconds);
		std::optional<FrameBufferView> Render();
		void SendMessageToWeb(std::string_view a_viewId, std::string_view a_json);
		void SetWebMessageHandler(WebMessageHandler a_handler);
		void SetLoadHandler(LoadHandler a_handler);
		void SetFailureHandler(FailureHandler a_handler);
		void SetCursorChangeHandler(CursorChangeHandler a_handler);
		void SetNativeAcceleratorHandler(NativeAcceleratorHandler a_handler);
		void SetNativeFocus(bool a_focused);
		void SetAcceleratorKeys(std::uint32_t a_toggleScan,
			bool a_captured, bool a_captureArmed, std::uint32_t a_captureUpScan);
		void SetSharedRingHandler(SharedRingHandler a_handler);
		void SetHealthHandler(HealthHandler a_handler);
		void InjectKeyEvent(std::uint32_t a_vkCode, bool a_down);
		void InjectMouseMove(int a_x, int a_y);
		void InjectMouseButton(int a_x, int a_y, int a_button, bool a_down);
		void InjectMouseWheel(int a_x, int a_y, int a_wheelDelta);
		void InjectPhysicalMouseWheel(int a_x, int a_y, int a_wheelDelta);
		void OpenDevTools(std::string_view a_viewId);
		void SetConsoleHandler(std::string_view a_viewId, ConsoleHandler a_handler);
		void SetViewHidden(std::string_view a_viewId, bool a_hidden);
		void SetViewOrder(std::string_view a_viewId, int a_order);
		void DestroyView(std::string_view a_viewId);

	private:
		struct Impl;
		std::unique_ptr<Impl> _impl;
	};
}
