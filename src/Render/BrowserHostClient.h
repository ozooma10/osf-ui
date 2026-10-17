#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include "Views/ViewManifest.h"
#include "BrowserBackend.h"

namespace osfui::browser::msg { struct Keyboard; struct TextInput; }

namespace OSFUI
{
	class SharedFrameConsumer;
	struct BrowserHostConfig
	{
		std::uint32_t width{ kDefaultViewWidth };
		std::uint32_t height{ kDefaultViewHeight };
		bool          devMode{ false };
		osfui::browser::Backend backend{ osfui::browser::Backend::WebView2 };
		std::function<std::optional<std::string>()> resolveLanguage;
		std::filesystem::path dataDir;
	};

	// Backend-neutral transport and browser lifetime. Both helpers implement the same private protocol.
	class BrowserHostClient
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
		using CursorChangeHandler = std::function<void(std::uint32_t a_id)>;
		using ConsoleHandler = std::function<void(int a_level, std::string a_message)>;
		using BackHandler = std::function<void(std::string_view a_viewId, std::uint64_t a_presentationEpoch)>;
		// DrainNotifications runs callbacks on the runtime tick; cursor callbacks use the transport thread.

		BrowserHostClient();
		~BrowserHostClient();

		bool Initialize(const BrowserHostConfig& a_config);
		// Runtime-thread call: prepare files asynchronously once per session, without starting a browser.
		void PrepareViewFiles();
		void RestartAfterFailure();
		void CreateOrNavigateView(const ViewManifest& a_manifest);
		bool RefreshModFiles(std::string_view a_mod);
		void SetInputTargetView(std::string_view a_id);
		void Resize(std::uint32_t a_width, std::uint32_t a_height);
		void SetViewport(std::uint32_t a_width, std::uint32_t a_height);
		// Discard captures from a canceled launcher presentation, including when HUDs remain shown.
		void ResetPresentation();
		void SetPointerInputEnabled(bool a_enabled);
		// Runtime update: callbacks run only here, before Runtime snapshots requests.
		void DrainNotifications();
		// Start a demanded host and acknowledge retired frames; does not dispatch callbacks.
		void Update();
		std::shared_ptr<SharedFrameConsumer> Frames() const;
		void SendMessageToWeb(std::string_view a_viewId, std::string_view a_json);
		void SetWebMessageHandler(WebMessageHandler a_handler);
		void SetLoadHandler(LoadHandler a_handler);
		void SetFailureHandler(FailureHandler a_handler);
		void SetCursorChangeHandler(CursorChangeHandler a_handler);
		void SetBackHandler(BackHandler a_handler);
		void RequestBack(std::string_view a_viewId);
		bool IsCurrentInputPresentation(std::string_view a_viewId, std::uint64_t a_epoch) const;
		void SetInputFocus(bool a_focused);
		void InjectKeyEvent(std::uint32_t a_vkCode, bool a_down);
		void InjectKeyboard(const osfui::browser::msg::Keyboard& a_key);
		void InjectText(const osfui::browser::msg::TextInput& a_text);
		void SetWindowActive(bool a_active);
		// Re-send OverlayInputHook::GameWindow() after the game recreates its window.
		void GameWindowChanged();
		void InjectMouseMove(int a_x, int a_y);
		void InjectMouseButton(int a_x, int a_y, int a_button, bool a_down);
		void InjectMouseWheel(int a_x, int a_y, int a_wheelDelta);
		void InjectPhysicalMouseWheel(int a_x, int a_y, int a_wheelDelta);
		void OpenDevTools(std::string_view a_viewId);
		void SetConsoleHandler(std::string_view a_viewId, ConsoleHandler a_handler);
		void SetViewHidden(std::string_view a_viewId, bool a_hidden, bool a_settleColdOpen = false);
		void SetViewOrder(std::string_view a_viewId, int a_order);
		void DestroyView(std::string_view a_viewId);

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
}
