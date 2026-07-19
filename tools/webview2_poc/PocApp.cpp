#include "PocApp.h"

#include <array>
#include <filesystem>
#include <format>
#include <string>

#include <windows.h>
#include <windowsx.h>

namespace
{
	constexpr wchar_t kWindowClass[] = L"OSFUI.WebView2.POC";
	constexpr UINT kCloseFromAccelerator = WM_APP + 41;

	std::filesystem::path FindViewsRoot()
	{
		const auto fromWorkingDirectory = std::filesystem::current_path() / "data" / "OSFUI" / "views";
		if (std::filesystem::exists(fromWorkingDirectory / "osfui" / "settings" / "index.html")) {
			return fromWorkingDirectory;
		}

		std::array<wchar_t, 32768> executable{};
		const auto length = ::GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
		auto cursor = std::filesystem::path(std::wstring_view(executable.data(), length)).parent_path();
		for (int depth = 0; depth < 8 && !cursor.empty(); ++depth) {
			const auto candidate = cursor / "data" / "OSFUI" / "views";
			if (std::filesystem::exists(candidate / "osfui" / "settings" / "index.html")) {
				return candidate;
			}
			cursor = cursor.parent_path();
		}
		return fromWorkingDirectory;
	}
}

int PocApp::Run(HINSTANCE a_instance, int a_showCommand)
{
	if (!CreateWindows(a_instance, a_showCommand)) {
		return 1;
	}

	RECT client{};
	::GetClientRect(_window, &client);
	const auto width = static_cast<std::uint32_t>((std::max)(1L, client.right - client.left));
	const auto height = static_cast<std::uint32_t>((std::max)(1L, client.bottom - client.top));
	if (!_d3d.Initialize(_window, width, height)) {
		::MessageBoxW(_window, L"Could not create the D3D11 stand-in.", L"OSF UI WebView2 POC", MB_ICONERROR);
		return 2;
	}

	_webView = std::make_unique<WebViewCapture>();
	const auto viewsRoot = FindViewsRoot();
	if (!_webView->Initialize(_window, _webHost, _d3d.Device(), _d3d.Context(), viewsRoot, width, height)) {
		::OutputDebugStringW(L"osfui-webview2-poc: WebView2 unavailable; continuing with the D3D11 stand-in only.\n");
	}

	RegisterRawInput();
	_start = std::chrono::steady_clock::now();
	_titleSample = _start;

	MSG message{};
	while (_running) {
		while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
			if (message.message == WM_QUIT) {
				_running = false;
				break;
			}
			::TranslateMessage(&message);
			::DispatchMessageW(&message);
		}
		if (!_running) {
			break;
		}

		if (_webView && _webView->CopyLatestFrame(_lastFrameSerial, _frame)) {
			_d3d.UploadFrame(_frame.pixels, _frame.width, _frame.height, _frame.stride);
		}
		const auto now = std::chrono::steady_clock::now();
		const auto seconds = std::chrono::duration<double>(now - _start).count();
		_d3d.Render(seconds);

		++_titleFrames;
		const auto titleElapsed = std::chrono::duration<double>(now - _titleSample).count();
		if (titleElapsed >= 1.0) {
			UpdateTitle(_titleFrames / titleElapsed);
			_titleFrames = 0;
			_titleSample = now;
		}
	}

	if (_webView) {
		_webView->Shutdown();
	}
	return 0;
}

bool PocApp::CreateWindows(HINSTANCE a_instance, int a_showCommand)
{
	WNDCLASSEXW windowClass{ sizeof(windowClass) };
	windowClass.style = CS_HREDRAW | CS_VREDRAW;
	windowClass.lpfnWndProc = &PocApp::StaticWndProc;
	windowClass.hInstance = a_instance;
	windowClass.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
	windowClass.hbrBackground = nullptr;
	windowClass.lpszClassName = kWindowClass;
	if (!::RegisterClassExW(&windowClass) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		return false;
	}

	_window = ::CreateWindowExW(0, kWindowClass, L"OSF UI WebView2 POC",
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 760,
		nullptr, nullptr, a_instance, this);
	if (!_window) {
		return false;
	}

	// The composition controller needs an HWND parent. It intentionally remains
	// visible for Win32 focus eligibility but only 1x1; pixels enter the
	// stand-in through Graphics Capture, not through this host HWND.
	_webHost = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
		0, 0, 1, 1, _window, nullptr, a_instance, nullptr);
	if (!_webHost) {
		return false;
	}

	::ShowWindow(_window, a_showCommand);
	::UpdateWindow(_window);
	return true;
}

LRESULT CALLBACK PocApp::StaticWndProc(HWND a_window, UINT a_message, WPARAM a_wparam, LPARAM a_lparam)
{
	PocApp* app = nullptr;
	if (a_message == WM_NCCREATE) {
		const auto create = reinterpret_cast<CREATESTRUCTW*>(a_lparam);
		app = static_cast<PocApp*>(create->lpCreateParams);
		::SetWindowLongPtrW(a_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
		app->_window = a_window;
	} else {
		app = reinterpret_cast<PocApp*>(::GetWindowLongPtrW(a_window, GWLP_USERDATA));
	}
	return app ? app->WndProc(a_window, a_message, a_wparam, a_lparam) :
		::DefWindowProcW(a_window, a_message, a_wparam, a_lparam);
}

LRESULT PocApp::WndProc(HWND a_window, UINT a_message, WPARAM a_wparam, LPARAM a_lparam)
{
	switch (a_message) {
	case WM_SIZE:
		if (a_wparam != SIZE_MINIMIZED) {
			OnResize(LOWORD(a_lparam), HIWORD(a_lparam));
		}
		return 0;

	case WM_INPUT:
		++_rawInputMessages;
		break;

	case WM_LBUTTONDOWN:
		if (_webView && _overlayVisible) {
			_webView->FocusWebView(false);
		}
		[[fallthrough]];
	case WM_LBUTTONUP:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_MOUSEMOVE:
	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL:
		if (_webView && _overlayVisible) {
			_webView->SendMouse(a_message, a_wparam, a_lparam);
			return 0;
		}
		break;

	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (a_wparam == VK_F10) {
			_overlayVisible = !_overlayVisible;
			if (_webView) {
			_d3d.SetOverlayVisible(_overlayVisible);
				_webView->SetVisible(_overlayVisible);
				if (_overlayVisible) {
					_webView->FocusWebView(false);
				} else {
					_webView->RestoreTopLevelFocus();
				}
			}
			return 0;
		}
		if (a_wparam == VK_ESCAPE && _overlayVisible) {
			_overlayVisible = false;
			_d3d.SetOverlayVisible(false);
			if (_webView) {
				_webView->SetVisible(false);
				_webView->RestoreTopLevelFocus();
			}
			return 0;
		}
		if (_webView) {
			switch (a_wparam) {
			case VK_F3:
				_webView->ExecuteScriptProbe();
				return 0;
			case VK_F4:
				_webView->SendBridgeProbe();
				return 0;
			case VK_F5:
				_webView->NavigateSettings();
				return 0;
			case VK_F6:
				_webView->NavigateMissing();
				return 0;
			case VK_F7:
				_webView->Recreate();
				return 0;
			case VK_F8:
				_webView->RunFocusCycleTest();
				return 0;
			case VK_F9:
				_webView->FocusWebView(true);
				return 0;
			default:
				break;
			}
		}
		break;

	case kCloseFromAccelerator:
		_overlayVisible = false;
		_d3d.SetOverlayVisible(false);
		if (_webView) {
			_webView->SetVisible(false);
			_webView->RestoreTopLevelFocus();
		}
		return 0;

	case WM_DESTROY:
		_running = false;
		::PostQuitMessage(0);
		return 0;

	default:
		break;
	}
	return ::DefWindowProcW(a_window, a_message, a_wparam, a_lparam);
}

void PocApp::OnResize(std::uint32_t a_width, std::uint32_t a_height)
{
	if (a_width == 0 || a_height == 0) {
		return;
	}
	_d3d.Resize(a_width, a_height);
	if (_webView) {
		_webView->Resize(a_width, a_height);
	}
}

void PocApp::UpdateTitle(double a_fps)
{
	const auto stats = _webView ? _webView->GetStats() : WebViewCapture::Stats{};
	const auto title = std::format(
		L"OSF UI WebView2 POC | {:.1f} fps | capture {} | readback {:.2f} ms avg {:.2f} | "
		L"bridge {:.2f} ms console {} http {} nav {}/{} cursor {} script {} alpha {}/{} | "
		L"raw {} keys {} focus-cycles {}/20 | active {} chrome-focus {} | value {} | "
		L"F2 inspect F10 toggle F8 x20 F9 widget fallback F3/F4 probes F5/F6 nav F7 recreate",
		a_fps, stats.captureFrames, stats.lastReadbackMs, stats.averageReadbackMs,
		stats.bridgeLatencyMs, stats.consoleMessages, stats.lastMissingHttpStatus,
		stats.lastNavigationSuccess ? L"ok" : L"FAIL", stats.lastNavigationError,
		stats.cursorId, stats.scriptProbeOk ? L"ok" : L"no",
		stats.translucentPixels, stats.premultiplyViolations,
		_rawInputMessages, stats.acceleratorEvents, stats.focusCyclesPassed,
		stats.topLevelActive ? L"yes" : L"NO", stats.chromeFocused ? L"yes" : L"no",
		stats.focusedValue.empty() ? L"<none>" : stats.focusedValue);
	::SetWindowTextW(_window, title.c_str());
}

void PocApp::RegisterRawInput()
{
	RAWINPUTDEVICE devices[2]{};
	devices[0] = { 0x01, 0x02, RIDEV_INPUTSINK, _window };  // mouse
	devices[1] = { 0x01, 0x06, RIDEV_INPUTSINK, _window };  // keyboard
	if (!::RegisterRawInputDevices(devices, static_cast<UINT>(std::size(devices)), sizeof(RAWINPUTDEVICE))) {
		::OutputDebugStringW(L"osfui-webview2-poc: RegisterRawInputDevices failed.\n");
	}
}
