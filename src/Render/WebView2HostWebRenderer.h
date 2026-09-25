#pragma once

#include <functional>
#include <optional>

#include "Input/CursorShape.h"
#include "Render/SharedTextureTransport.h"
#include "Views/ViewManifest.h"

namespace osfui::wv2::msg { struct Keyboard; struct TextInput; }

namespace OSFUI
{
	class SharedFrameConsumer;
	struct WebView2HostConfig
	{
		std::uint32_t width{ kDefaultViewWidth };
		std::uint32_t height{ kDefaultViewHeight };
		bool          devMode{ false };
		std::function<std::optional<std::string>()> resolveLanguage;
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
			int              errorCode{ 0 };
		};

		struct FailureEvent
		{
			std::string_view stage;
			std::string_view viewId;
			std::string_view description;
			std::uint32_t    errorCode{ 0 };
		};

		using WebMessageHandler = std::function<void(std::string_view a_viewId, std::string_view a_json)>;
		using LoadHandler = std::function<void(const LoadEvent& a_event)>;
		using FailureHandler = std::function<void(const FailureEvent& a_event)>;
		using CursorChangeHandler = std::function<void(CursorShape a_shape)>;
		using ConsoleHandler = std::function<void(int a_level, std::string a_message)>;
		// Update drains game-thread callbacks; cursor callbacks run on the transport thread.

		WebView2HostWebRenderer();
		~WebView2HostWebRenderer();

		bool Initialize(const WebView2HostConfig& a_config);
		void RestartAfterFailure();
		void CreateOrNavigateView(const ViewManifest& a_manifest);
		bool RefreshViewFiles(std::string_view a_viewId);
		void SetInputTargetView(std::string_view a_id);
		void Resize(std::uint32_t a_width, std::uint32_t a_height);
		void SetViewport(std::uint32_t a_width, std::uint32_t a_height);
		void SetPointerInputEnabled(bool a_enabled);
		// Main thread: callbacks run only here, before Runtime snapshots requests.
		void DrainNotifications();
		// Start a demanded host and acknowledge retired frames; does not dispatch callbacks.
		void Update();
		std::shared_ptr<SharedFrameConsumer> Frames() const;
		void SendMessageToWeb(std::string_view a_viewId, std::string_view a_json);
		void SetWebMessageHandler(WebMessageHandler a_handler);
		void SetLoadHandler(LoadHandler a_handler);
		void SetFailureHandler(FailureHandler a_handler);
		void SetCursorChangeHandler(CursorChangeHandler a_handler);
		void SetInputFocus(bool a_focused);
		void InjectKeyEvent(std::uint32_t a_vkCode, bool a_down);
		void InjectKeyboard(const osfui::wv2::msg::Keyboard& a_key);
		void InjectText(const osfui::wv2::msg::TextInput& a_text);
		void SetWindowActive(bool a_active);
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
		std::unique_ptr<Impl> m_impl;
	};
}
