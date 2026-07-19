#pragma once

#include "D3DStandIn.h"
#include "WebViewCapture.h"

#include <chrono>
#include <cstdint>
#include <memory>

class PocApp
{
public:
	int Run(HINSTANCE a_instance, int a_showCommand);

private:
	static LRESULT CALLBACK StaticWndProc(HWND a_window, UINT a_message, WPARAM a_wparam, LPARAM a_lparam);
	LRESULT WndProc(HWND a_window, UINT a_message, WPARAM a_wparam, LPARAM a_lparam);
	bool CreateWindows(HINSTANCE a_instance, int a_showCommand);
	void OnResize(std::uint32_t a_width, std::uint32_t a_height);
	void UpdateTitle(double a_fps);
	void RegisterRawInput();

	HWND _window{ nullptr };
	HWND _webHost{ nullptr };
	D3DStandIn _d3d;
	std::unique_ptr<WebViewCapture> _webView;
	WebViewCapture::Frame _frame;
	std::uint64_t _lastFrameSerial{ 0 };
	std::uint64_t _rawInputMessages{ 0 };
	bool _running{ true };
	bool _overlayVisible{ true };
	std::chrono::steady_clock::time_point _start;
	std::chrono::steady_clock::time_point _titleSample;
	std::uint32_t _titleFrames{ 0 };
};
