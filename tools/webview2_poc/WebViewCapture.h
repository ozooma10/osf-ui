#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <d3d11.h>

class WebViewCapture
{
public:
	struct Frame
	{
		std::vector<std::uint8_t> pixels;
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		std::uint32_t stride{ 0 };
		std::uint64_t serial{ 0 };
		double readbackMs{ 0.0 };
	};

	struct Stats
	{
		std::uint64_t captureFrames{ 0 };
		double lastReadbackMs{ 0.0 };
		double averageReadbackMs{ 0.0 };
		std::uint64_t webMessages{ 0 };
		std::uint64_t consoleMessages{ 0 };
		std::uint32_t focusCyclesPassed{ 0 };
		std::uint64_t acceleratorEvents{ 0 };
		std::wstring focusedValue;
		double bridgeLatencyMs{ -1.0 };
		int lastMissingHttpStatus{ 0 };
		bool lastNavigationSuccess{ false };
		int lastNavigationError{ 0 };
		std::uint64_t translucentPixels{ 0 };
		std::uint64_t premultiplyViolations{ 0 };
		std::uint32_t cursorId{ 0 };
		bool scriptProbeOk{ false };
		bool ready{ false };
		bool domReady{ false };
		bool topLevelActive{ false };
		bool chromeFocused{ false };
	};

	WebViewCapture();
	~WebViewCapture();

	bool Initialize(HWND a_topLevel, HWND a_hostWindow, ID3D11Device* a_device,
		ID3D11DeviceContext* a_context, std::filesystem::path a_viewsRoot,
		std::uint32_t a_width, std::uint32_t a_height);
	void Shutdown();
	void Resize(std::uint32_t a_width, std::uint32_t a_height);
	void SetVisible(bool a_visible);
	void FocusWebView(bool a_allowWidgetFallback);
	void RestoreTopLevelFocus();
	void RunFocusCycleTest();
	bool Recreate();
	void NavigateSettings();
	void NavigateMissing();
	void SendBridgeProbe();
	void ExecuteScriptProbe();
	void SendMouse(UINT a_message, WPARAM a_wparam, LPARAM a_lparam);
	[[nodiscard]] bool CopyLatestFrame(std::uint64_t& a_lastSerial, Frame& a_frame);
	[[nodiscard]] Stats GetStats() const;
	[[nodiscard]] bool IsReady() const;
	[[nodiscard]] bool IsVisible() const;

private:
	struct Impl;
	std::unique_ptr<Impl> _impl;
};
