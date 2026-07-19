#include "WebViewCapture.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <string_view>

#include <windows.h>
#include <windowsx.h>
#include <DispatcherQueue.h>
#include <ShlObj.h>
#include <WebView2.h>
#include <wrl.h>
#include <wrl/client.h>

#include <d3d10.h>
#include <d3d11.h>
#include <dxgi.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/base.h>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using namespace winrt;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using namespace winrt::Windows::UI::Composition;

namespace
{
	constexpr UINT kCloseFromAccelerator = WM_APP + 41;

	void Log(std::wstring_view a_message)
	{
		std::wstring line(a_message);
		line += L"\n";
		::OutputDebugStringW(line.c_str());
	}

	void LogHr(std::wstring_view a_where, HRESULT a_hr)
	{
		Log(std::format(L"osfui-webview2-poc: {} failed (0x{:08X})", a_where,
			static_cast<unsigned>(a_hr)));
	}

	std::filesystem::path UserDataFolder()
	{
		PWSTR localAppData = nullptr;
		if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData))) {
			return std::filesystem::temp_directory_path() / "OSFUI" / "WebView2";
		}
		std::filesystem::path result(localAppData);
		::CoTaskMemFree(localAppData);
		return result / "OSFUI" / "WebView2";
	}

	std::optional<std::wstring> JsonStringField(std::wstring_view a_json, std::wstring_view a_field)
	{
		const auto needle = std::wstring(L"\"") + std::wstring(a_field) + L"\":\"";
		const auto begin = a_json.find(needle);
		if (begin == std::wstring_view::npos) {
			return std::nullopt;
		}
		const auto valueBegin = begin + needle.size();
		const auto end = a_json.find(L'"', valueBegin);
		if (end == std::wstring_view::npos) {
			return std::nullopt;
		}
		return std::wstring(a_json.substr(valueBegin, end - valueBegin));
	}

	std::optional<double> JsonNumberField(std::wstring_view a_json, std::wstring_view a_field)
	{
		const auto needle = std::wstring(L"\"") + std::wstring(a_field) + L"\":";
		const auto begin = a_json.find(needle);
		if (begin == std::wstring_view::npos) {
			return std::nullopt;
		}
		const auto text = std::wstring(a_json.substr(begin + needle.size()));
		wchar_t* end = nullptr;
		const auto value = std::wcstod(text.c_str(), &end);
		return end != text.c_str() ? std::optional<double>(value) : std::nullopt;
	}
	HWND FindChromeWidget(HWND a_parent)
	{
		struct Search
		{
			HWND found{ nullptr };
		} search;
		::EnumChildWindows(a_parent, [](HWND a_window, LPARAM a_param) -> BOOL {
			auto* state = reinterpret_cast<Search*>(a_param);
			wchar_t className[128]{};
			::GetClassNameW(a_window, className, static_cast<int>(std::size(className)));
			if (std::wstring_view(className).starts_with(L"Chrome_WidgetWin_")) {
				state->found = a_window;
				return FALSE;
			}
			return TRUE;
		}, reinterpret_cast<LPARAM>(&search));
		return search.found;
	}

	bool IsChromeWidget(HWND a_window)
	{
		if (!a_window) {
			return false;
		}
		wchar_t className[128]{};
		::GetClassNameW(a_window, className, static_cast<int>(std::size(className)));
		return std::wstring_view(className).starts_with(L"Chrome_WidgetWin_");
	}
}

struct WebViewCapture::Impl
{
	HWND topLevel{ nullptr };
	HWND hostWindow{ nullptr };
	std::filesystem::path viewsRoot;
	std::filesystem::path userData;
	std::uint32_t width{ 1 };
	std::uint32_t height{ 1 };
	bool visible{ true };

	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	std::array<ComPtr<ID3D11Texture2D>, 3> staging;
	std::uint32_t stagingWriteIndex{ 0 };
	std::uint32_t stagedFrameCount{ 0 };
	std::uint32_t stagingWidth{ 0 };
	std::uint32_t stagingHeight{ 0 };
	Frame scratch;

	winrt::Windows::System::DispatcherQueueController dispatcher{ nullptr };
	Compositor compositor{ nullptr };
	ContainerVisual rootVisual{ nullptr };
	IDirect3DDevice captureDevice{ nullptr };
	GraphicsCaptureItem captureItem{ nullptr };
	Direct3D11CaptureFramePool framePool{ nullptr };
	GraphicsCaptureSession captureSession{ nullptr };
	event_token frameToken{};

	ComPtr<ICoreWebView2Environment> environment;
	ComPtr<ICoreWebView2Controller> controller;
	ComPtr<ICoreWebView2CompositionController> compositionController;
	ComPtr<ICoreWebView2> webView;
	ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> consoleReceiver;

	::EventRegistrationToken acceleratorToken{};
	::EventRegistrationToken messageToken{};
	::EventRegistrationToken navigationToken{};
	::EventRegistrationToken domToken{};
	::EventRegistrationToken processFailedToken{};
	::EventRegistrationToken consoleToken{};

	mutable std::mutex frameMutex;
	mutable std::mutex captureMutex;
	::EventRegistrationToken cursorToken{};
	::EventRegistrationToken resourceResponseToken{};
	Frame latest;
	std::atomic_bool shuttingDown{ false };
	std::atomic_bool ready{ false };
	std::atomic_bool domReady{ false };
	std::atomic_uint64_t captureFrames{ 0 };
	std::atomic_uint64_t webMessages{ 0 };
	std::atomic_uint64_t consoleMessages{ 0 };
	std::atomic_uint32_t focusCyclesPassed{ 0 };
	std::atomic_uint64_t acceleratorEvents{ 0 };
	std::wstring focusedValue;
	std::atomic<double> lastReadbackMs{ 0.0 };
	std::atomic<double> bridgeLatencyMs{ -1.0 };
	std::atomic_int lastMissingHttpStatus{ 0 };
	std::atomic_bool lastNavigationSuccess{ false };
	std::atomic_int lastNavigationError{ 0 };
	std::atomic_uint64_t translucentPixels{ 0 };
	std::atomic_uint64_t premultiplyViolations{ 0 };
	std::atomic_uint32_t cursorId{ 0 };
	std::atomic_bool scriptProbeOk{ false };
	std::atomic<double> totalReadbackMs{ 0.0 };
	std::atomic_uint64_t measuredFrames{ 0 };
	bool alphaReported{ false };

	bool InitializeCore()
	{
		init_apartment(apartment_type::single_threaded);

		DispatcherQueueOptions queueOptions{
			sizeof(DispatcherQueueOptions),
			DQTYPE_THREAD_CURRENT,
			DQTAT_COM_STA
		};
		check_hresult(::CreateDispatcherQueueController(queueOptions,
			reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(put_abi(dispatcher))));

		compositor = Compositor();
		rootVisual = compositor.CreateContainerVisual();
		rootVisual.Size({ static_cast<float>(width), static_cast<float>(height) });

		ComPtr<IDXGIDevice> dxgiDevice;
		check_hresult(device.As(&dxgiDevice));
		winrt::com_ptr<::IInspectable> inspectable;
		check_hresult(::CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()));
		wchar_t forcedAbsent[2]{};
		if (::GetEnvironmentVariableW(L"OSFUI_WEBVIEW2_POC_FORCE_RUNTIME_ABSENT", forcedAbsent, 2) > 0) {
			Log(L"osfui-webview2-poc: forced runtime-absent fallback");
			return false;
		}
		captureDevice = inspectable.as<IDirect3DDevice>();

		return BeginEnvironment();
	}

	bool BeginEnvironment()
	{
		LPWSTR version = nullptr;
		const auto detect = ::GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
		if (FAILED(detect) || !version) {
			LogHr(L"WebView2 runtime detection", detect);
			return false;
		}
		Log(std::format(L"osfui-webview2-poc: WebView2 runtime {}", version));
		::CoTaskMemFree(version);

		std::error_code ec;
		std::filesystem::create_directories(userData, ec);
		shuttingDown.store(false);
		ready.store(false);
		domReady.store(false);

		const auto callback = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
			[this](HRESULT a_result, ICoreWebView2Environment* a_environment) -> HRESULT {
				if (shuttingDown.load()) {
					return S_OK;
				}
				if (FAILED(a_result) || !a_environment) {
					LogHr(L"CreateCoreWebView2EnvironmentWithOptions callback", a_result);
					return S_OK;
				}
				environment = a_environment;
				ComPtr<ICoreWebView2Environment3> environment3;
				auto hr = environment.As(&environment3);
				if (FAILED(hr)) {
					LogHr(L"QI ICoreWebView2Environment3", hr);
					return S_OK;
				}
				hr = environment3->CreateCoreWebView2CompositionController(hostWindow,
					Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
						[this](HRESULT a_controllerResult,
							ICoreWebView2CompositionController* a_compositionController) -> HRESULT {
							return OnController(a_controllerResult, a_compositionController);
						}).Get());
				if (FAILED(hr)) {
					LogHr(L"CreateCoreWebView2CompositionController", hr);
				}
				return S_OK;
			});

		const auto hr = ::CreateCoreWebView2EnvironmentWithOptions(
			nullptr, userData.c_str(), nullptr, callback.Get());
		if (FAILED(hr)) {
			LogHr(L"CreateCoreWebView2EnvironmentWithOptions", hr);
			return false;
		}
		return true;
	}

	HRESULT OnController(HRESULT a_result, ICoreWebView2CompositionController* a_compositionController)
	{
		if (shuttingDown.load()) {
			return S_OK;
		}
		if (FAILED(a_result) || !a_compositionController) {
			LogHr(L"composition controller callback", a_result);
			return S_OK;
		}
		compositionController = a_compositionController;
		auto hr = compositionController.As(&controller);
		if (FAILED(hr)) {
			LogHr(L"QI ICoreWebView2Controller", hr);
			return S_OK;
		}
		hr = controller->get_CoreWebView2(&webView);
		if (FAILED(hr) || !webView) {
			LogHr(L"get_CoreWebView2", hr);
			return S_OK;
		}

		RECT bounds{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
		controller->put_Bounds(bounds);
		controller->put_IsVisible(visible ? TRUE : FALSE);

		ComPtr<ICoreWebView2Controller2> controller2;
		if (SUCCEEDED(controller.As(&controller2))) {
			controller2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{ 0, 0, 0, 0 });
		}

		const auto rootUnknown = rootVisual.as<::IUnknown>();
		hr = compositionController->put_RootVisualTarget(rootUnknown.get());
		if (FAILED(hr)) {
			LogHr(L"put_RootVisualTarget", hr);
			return S_OK;
		}

		ComPtr<ICoreWebView2_3> webView3;
		hr = webView.As(&webView3);
		if (FAILED(hr)) {
			LogHr(L"QI ICoreWebView2_3", hr);
			return S_OK;
		}
		hr = webView3->SetVirtualHostNameToFolderMapping(
			L"osfui.local", viewsRoot.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
		if (FAILED(hr)) {
			LogHr(L"SetVirtualHostNameToFolderMapping", hr);
			return S_OK;
		}

		InstallEvents();

		static constexpr wchar_t kBridgeShim[] = LR"(
			(() => {
				const bridge = window.osfui = window.osfui || {};
				bridge.postMessage = (json) => chrome.webview.postMessage(String(json));
				chrome.webview.addEventListener('message', (event) => {
					const json = typeof event.data === 'string' ? event.data : JSON.stringify(event.data);
					if (window.osfui && typeof window.osfui.onMessage === 'function') {
						window.osfui.onMessage(json);
					}
				});
			})();)";

		hr = webView->AddScriptToExecuteOnDocumentCreated(kBridgeShim,
			Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
				[](HRESULT a_scriptResult, LPCWSTR) -> HRESULT {
					if (FAILED(a_scriptResult)) {
						LogHr(L"AddScriptToExecuteOnDocumentCreated", a_scriptResult);
					}
					return S_OK;
				}).Get());
		if (FAILED(hr)) {
			LogHr(L"AddScriptToExecuteOnDocumentCreated", hr);
		}

		if (!StartCapture()) {
			return S_OK;
		}
		ready.store(true);
		NavigateSettings();
		Log(L"osfui-webview2-poc: composition controller and visual capture are ready");
		return S_OK;
	}

	void InstallEvents()
	{
		compositionController->add_CursorChanged(
			Callback<ICoreWebView2CursorChangedEventHandler>(
				[this](ICoreWebView2CompositionController* a_sender, ::IUnknown*) -> HRESULT {
					UINT32 id = 0;
					if (SUCCEEDED(a_sender->get_SystemCursorId(&id))) {
						cursorId.store(id);
					}
					return S_OK;
				}).Get(), &cursorToken);
		controller->add_AcceleratorKeyPressed(
			Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
				[this](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* a_args) -> HRESULT {
					UINT key = 0;
					COREWEBVIEW2_KEY_EVENT_KIND kind{};
					a_args->get_VirtualKey(&key);
					a_args->get_KeyEventKind(&kind);
					acceleratorEvents.fetch_add(1);
					const bool down = kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
						kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN;
					if (down && (key == VK_F10 || key == VK_ESCAPE)) {
						a_args->put_Handled(TRUE);
						::PostMessageW(topLevel, kCloseFromAccelerator, 0, 0);
					} else if (down && key == VK_F2) {
						a_args->put_Handled(TRUE);
						webView->ExecuteScript(
							L"JSON.stringify({tag:document.activeElement && document.activeElement.tagName,"
							L"value:document.activeElement && document.activeElement.value || ''})",
							Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
								[this](HRESULT a_result, LPCWSTR a_json) -> HRESULT {
									focusedValue = SUCCEEDED(a_result) && a_json ? a_json : L"<probe-failed>";
									return S_OK;
								}).Get());
					}
					return S_OK;
				}).Get(), &acceleratorToken);

		webView->add_WebMessageReceived(
			Callback<ICoreWebView2WebMessageReceivedEventHandler>(
				[this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a_args) -> HRESULT {
					LPWSTR raw = nullptr;
					if (FAILED(a_args->TryGetWebMessageAsString(&raw)) || !raw) {
						return S_OK;
					}
					std::wstring message(raw);
					::CoTaskMemFree(raw);
					++webMessages;
					if (JsonStringField(message, L"type") == L"poc.bridgeAck") {
						if (const auto latency = JsonNumberField(message, L"latencyMs")) {
							bridgeLatencyMs.store(*latency);
						}
					}
					Log(std::format(L"osfui-webview2-poc: web message {}", message));
					ReplyToBridge(message);
					return S_OK;
				}).Get(), &messageToken);

		webView->add_NavigationCompleted(
			Callback<ICoreWebView2NavigationCompletedEventHandler>(
				[this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* a_args) -> HRESULT {
					BOOL success = FALSE;
					COREWEBVIEW2_WEB_ERROR_STATUS status{};
					a_args->get_IsSuccess(&success);
					a_args->get_WebErrorStatus(&status);
					lastNavigationSuccess.store(success == TRUE);
					lastNavigationError.store(static_cast<int>(status));
					Log(std::format(L"osfui-webview2-poc: navigation {} (status {})",
						success ? L"completed" : L"FAILED", static_cast<int>(status)));
					return S_OK;
				}).Get(), &navigationToken);

		ComPtr<ICoreWebView2_2> webView2;
		if (SUCCEEDED(webView.As(&webView2))) {
			webView2->add_WebResourceResponseReceived(
				Callback<ICoreWebView2WebResourceResponseReceivedEventHandler>(
					[this](ICoreWebView2*, ICoreWebView2WebResourceResponseReceivedEventArgs* a_args) -> HRESULT {
						ComPtr<ICoreWebView2WebResourceRequest> request;
						ComPtr<ICoreWebView2WebResourceResponseView> response;
						LPWSTR uri = nullptr;
						int status = 0;
						if (SUCCEEDED(a_args->get_Request(&request)) && request &&
							SUCCEEDED(request->get_Uri(&uri)) && uri &&
							std::wstring_view(uri).find(L"__deliberate_404__") != std::wstring_view::npos &&
							SUCCEEDED(a_args->get_Response(&response)) && response &&
							SUCCEEDED(response->get_StatusCode(&status))) {
							lastMissingHttpStatus.store(status);
						}
						if (uri) {
							::CoTaskMemFree(uri);
						}
						return S_OK;
					}).Get(), &resourceResponseToken);
			webView2->add_DOMContentLoaded(
				Callback<ICoreWebView2DOMContentLoadedEventHandler>(
					[this](ICoreWebView2*, ICoreWebView2DOMContentLoadedEventArgs*) -> HRESULT {
						domReady.store(true);
						webView->PostWebMessageAsString(
							L"{\"type\":\"runtime.ready\",\"payload\":{\"version\":\"webview2-poc\"}}");
						webView->ExecuteScript(
							LR"((() => {
								if (document.getElementById('osfui-poc-motion')) return;
								const marker = document.createElement('div');
								marker.id = 'osfui-poc-motion';
								marker.style.cssText = 'position:fixed;z-index:2147483647;left:0;top:0;width:96px;height:4px;pointer-events:none;background:rgba(80,210,255,.45)';
								document.documentElement.appendChild(marker);
								marker.animate([{transform:'translateX(0px)'},{transform:'translateX(calc(100vw - 96px))'}],
									{duration:900,direction:'alternate',iterations:Infinity,easing:'linear'});
							})())",
							Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
								[](HRESULT, LPCWSTR) -> HRESULT { return S_OK; }).Get());
						Log(L"osfui-webview2-poc: DOM ready");
						return S_OK;
					}).Get(), &domToken);
		}

		webView->add_ProcessFailed(
			Callback<ICoreWebView2ProcessFailedEventHandler>(
				[](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* a_args) -> HRESULT {
					COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
					a_args->get_ProcessFailedKind(&kind);
					Log(std::format(L"osfui-webview2-poc: PROCESS FAILED kind {}", static_cast<int>(kind)));
					return S_OK;
				}).Get(), &processFailedToken);

		if (SUCCEEDED(webView->GetDevToolsProtocolEventReceiver(
			L"Runtime.consoleAPICalled", &consoleReceiver)) && consoleReceiver) {
			consoleReceiver->add_DevToolsProtocolEventReceived(
				Callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
					[this](ICoreWebView2*,
						ICoreWebView2DevToolsProtocolEventReceivedEventArgs* a_args) -> HRESULT {
						LPWSTR json = nullptr;
						if (SUCCEEDED(a_args->get_ParameterObjectAsJson(&json)) && json) {
							++consoleMessages;
							Log(std::format(L"osfui-webview2-poc: console {}", json));
							::CoTaskMemFree(json);
						}
						return S_OK;
					}).Get(), &consoleToken);
			webView->CallDevToolsProtocolMethod(L"Runtime.enable", L"{}",
				Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
					[](HRESULT, LPCWSTR) -> HRESULT { return S_OK; }).Get());
		}
	}

	bool StartCapture()
	{
		try {
			captureItem = GraphicsCaptureItem::CreateFromVisual(rootVisual);
			const SizeInt32 size{ static_cast<int32_t>(width), static_cast<int32_t>(height) };
			framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
				captureDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, size);
			frameToken = framePool.FrameArrived([this](Direct3D11CaptureFramePool const& a_pool,
				winrt::Windows::Foundation::IInspectable const&) {
				OnFrameArrived(a_pool);
			});
			captureSession = framePool.CreateCaptureSession(captureItem);
			try {
				captureSession.IsCursorCaptureEnabled(false);
			} catch (...) {
			}
			captureSession.StartCapture();
			return true;
		} catch (const hresult_error& error) {
			Log(std::format(L"osfui-webview2-poc: Graphics Capture setup failed: {} (0x{:08X})",
				error.message().c_str(), static_cast<unsigned>(error.code())));
			return false;
		}
	}

	void OnFrameArrived(const Direct3D11CaptureFramePool& a_pool)
	{
		if (shuttingDown.load()) {
			return;
		}
		const auto started = std::chrono::steady_clock::now();
		try {
			auto frame = a_pool.TryGetNextFrame();
			if (!frame) {
				return;
			}
			auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
			ComPtr<ID3D11Texture2D> source;
			check_hresult(access->GetInterface(IID_PPV_ARGS(&source)));

			D3D11_TEXTURE2D_DESC sourceDesc{};
			source->GetDesc(&sourceDesc);
			std::scoped_lock resourceLock(captureMutex);
			if (!staging[0] || sourceDesc.Width != stagingWidth || sourceDesc.Height != stagingHeight) {
				D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
				stagingDesc.BindFlags = 0;
				stagingDesc.MiscFlags = 0;
				stagingDesc.Usage = D3D11_USAGE_STAGING;
				stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				for (auto& texture : staging) {
					texture.Reset();
					check_hresult(device->CreateTexture2D(&stagingDesc, nullptr, &texture));
				}
				stagingWidth = sourceDesc.Width;
				stagingHeight = sourceDesc.Height;
				stagingWriteIndex = 0;
				stagedFrameCount = 0;
				scratch.pixels.resize(static_cast<std::size_t>(sourceDesc.Width) * sourceDesc.Height * 4);
			}

			context->CopyResource(staging[stagingWriteIndex].Get(), source.Get());
			if (stagedFrameCount < 2) {
				++stagedFrameCount;
				stagingWriteIndex = (stagingWriteIndex + 1) % staging.size();
				return;
			}
			const auto readIndex = (stagingWriteIndex + 1) % staging.size();
			stagingWriteIndex = (stagingWriteIndex + 1) % staging.size();

			D3D11_MAPPED_SUBRESOURCE mapped{};
			const auto mapResult = context->Map(staging[readIndex].Get(), 0, D3D11_MAP_READ, 0, &mapped);
			if (FAILED(mapResult)) {
				LogHr(L"Map(staging ring)", mapResult);
				return;
			}

			scratch.width = sourceDesc.Width;
			scratch.height = sourceDesc.Height;
			scratch.stride = sourceDesc.Width * 4;
			scratch.pixels.resize(static_cast<std::size_t>(scratch.stride) * scratch.height);
			const auto* sourceBytes = static_cast<const std::uint8_t*>(mapped.pData);
			for (std::uint32_t row = 0; row < scratch.height; ++row) {
				std::memcpy(scratch.pixels.data() + static_cast<std::size_t>(row) * scratch.stride,
					sourceBytes + static_cast<std::size_t>(row) * mapped.RowPitch, scratch.stride);
			}
			context->Unmap(staging[readIndex].Get(), 0);

			const auto elapsed = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - started).count();
			const auto serial = captureFrames.fetch_add(1) + 1;
			scratch.serial = serial;
			scratch.readbackMs = elapsed;
			lastReadbackMs.store(elapsed);
			if (serial > 30) {
				measuredFrames.fetch_add(1);
				totalReadbackMs.fetch_add(elapsed);
			}

			if (!alphaReported) {
				ReportAlpha(scratch);
				alphaReported = translucentPixels.load() > 0;
			}
			std::scoped_lock frameLock(frameMutex);
			latest.pixels.swap(scratch.pixels);
			latest.width = scratch.width;
			latest.height = scratch.height;
			latest.stride = scratch.stride;
			latest.serial = scratch.serial;
			latest.readbackMs = scratch.readbackMs;
		} catch (const hresult_error& error) {
			Log(std::format(L"osfui-webview2-poc: FrameArrived failed: {} (0x{:08X})",
				error.message().c_str(), static_cast<unsigned>(error.code())));
		}
	}

	void ReportAlpha(const Frame& a_frame)
	{
		std::size_t translucent = 0;
		std::size_t violationCount = 0;
		for (std::size_t offset = 0; offset + 3 < a_frame.pixels.size(); offset += 4) {
			const auto b = a_frame.pixels[offset + 0];
			const auto g = a_frame.pixels[offset + 1];
			const auto r = a_frame.pixels[offset + 2];
			const auto alpha = a_frame.pixels[offset + 3];
			if (alpha > 0 && alpha < 255) {
				++translucent;
				if (b > alpha + 1 || g > alpha + 1 || r > alpha + 1) {
					++violationCount;
				}
			}
		}
		translucentPixels.store(translucent);
		premultiplyViolations.store(violationCount);
		Log(std::format(L"osfui-webview2-poc: alpha sample translucent={} premultiply-violations={}",
			translucent, violationCount));
	}

	void ReplyToBridge(std::wstring_view a_message)
	{
		if (!webView) {
			return;
		}
		const auto requestId = JsonStringField(a_message, L"requestId");
		const auto command = JsonStringField(a_message, L"command");
		const auto suffix = requestId ? std::format(L",\"requestId\":\"{}\"", *requestId) : L"";

		if (command && *command == L"i18n.get") {
			const auto reply = std::format(
				L"{{\"type\":\"i18n.data\",\"payload\":{{\"locale\":\"en\",\"strings\":{{}}}}{}}}", suffix);
			webView->PostWebMessageAsString(reply.c_str());
		} else if (command && *command == L"settings.get") {
			webView->PostWebMessageAsString(
				L"{\"type\":\"settings.data\",\"payload\":{\"mods\":[],\"loadErrors\":[]}}");
		} else if (command && *command == L"views.get") {
			webView->PostWebMessageAsString(
				L"{\"type\":\"views.data\",\"payload\":{\"views\":[]}}");
		} else if (requestId) {
			const auto reply = std::format(
				L"{{\"type\":\"ui.result\",\"payload\":{{\"ok\":true,\"command\":\"{}\"" L"}}{}}}",
				command.value_or(L"poc.unknown"), suffix);
			webView->PostWebMessageAsString(reply.c_str());
		}
	}

	void NavigateSettings()
	{
		if (webView) {
			domReady.store(false);
			webView->Navigate(L"https://osfui.local/osfui/settings/index.html");
		}
	}

	void StopWebResources()
	{
		shuttingDown.store(true);
		ready.store(false);
		domReady.store(false);

		if (framePool) {
			try {
				framePool.FrameArrived(frameToken);
			} catch (...) {
			}
		}
		if (captureSession) {
			captureSession.Close();
			captureSession = nullptr;
		}
		if (framePool) {
			framePool.Close();
			framePool = nullptr;
		}
		captureItem = nullptr;
		for (auto& texture : staging) {
			texture.Reset();
		}
		stagingWidth = 0;
		stagingHeight = 0;
		stagedFrameCount = 0;

		if (compositionController) {
			compositionController->put_RootVisualTarget(nullptr);
		}
		if (controller) {
			controller->Close();
		}
		consoleReceiver.Reset();
		webView.Reset();
		compositionController.Reset();
		controller.Reset();
		environment.Reset();
	}
};
WebViewCapture::WebViewCapture() :
	_impl(std::make_unique<Impl>())
{}

WebViewCapture::~WebViewCapture()
{
	Shutdown();
}

bool WebViewCapture::Initialize(HWND a_topLevel, HWND a_hostWindow, ID3D11Device* a_device,
	ID3D11DeviceContext* a_context, std::filesystem::path a_viewsRoot,
	std::uint32_t a_width, std::uint32_t a_height)
{
	if (!a_topLevel || !a_hostWindow || !a_device || !a_context) {
		return false;
	}
	_impl->topLevel = a_topLevel;
	_impl->hostWindow = a_hostWindow;
	// Keep capture/readback off the stand-in render device. Sharing an immediate
	// context makes Map contend with a vsync-blocked Present and measures that
	// lock rather than the WGC readback path used by the eventual backend.
	const D3D_FEATURE_LEVEL requested[] = {
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
	};
	D3D_FEATURE_LEVEL actual{};
	auto deviceResult = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
		D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested, static_cast<UINT>(std::size(requested)),
		D3D11_SDK_VERSION, &_impl->device, &actual, &_impl->context);
	if (FAILED(deviceResult)) {
		LogHr(L"D3D11CreateDevice(capture)", deviceResult);
		return false;
	}
	ComPtr<ID3D10Multithread> multithread;
	if (SUCCEEDED(_impl->context.As(&multithread))) {
		multithread->SetMultithreadProtected(TRUE);
	}
	_impl->viewsRoot = std::move(a_viewsRoot);
	_impl->userData = UserDataFolder();
	_impl->width = (std::max)(1u, a_width);
	_impl->height = (std::max)(1u, a_height);

	if (!std::filesystem::exists(_impl->viewsRoot / "osfui" / "settings" / "index.html")) {
		Log(std::format(L"osfui-webview2-poc: real settings view not found under {}",
			_impl->viewsRoot.wstring()));
		return false;
	}

	try {
		return _impl->InitializeCore();
	} catch (const hresult_error& error) {
		Log(std::format(L"osfui-webview2-poc: initialization failed: {} (0x{:08X})",
			error.message().c_str(), static_cast<unsigned>(error.code())));
		return false;
	}
}

void WebViewCapture::Shutdown()
{
	if (!_impl) {
		return;
	}
	_impl->StopWebResources();
	if (_impl->dispatcher) {
		try {
			_impl->dispatcher.ShutdownQueueAsync();
		} catch (...) {
		}
		_impl->dispatcher = nullptr;
	}
	_impl->rootVisual = nullptr;
	_impl->compositor = nullptr;
	_impl->captureDevice = nullptr;
	_impl->context.Reset();
	_impl->device.Reset();
}

bool WebViewCapture::Recreate()
{
	if (!_impl->compositor || !_impl->device) {
		return false;
	}
	Log(L"osfui-webview2-poc: destroy/recreate requested");
	_impl->StopWebResources();
	{
		std::scoped_lock lock(_impl->frameMutex);
		_impl->latest = {};
	}
	_impl->alphaReported = false;
	return _impl->BeginEnvironment();
}

void WebViewCapture::Resize(std::uint32_t a_width, std::uint32_t a_height)
{
	if (a_width == 0 || a_height == 0) {
		return;
	}
	_impl->width = a_width;
	_impl->height = a_height;
	if (_impl->rootVisual) {
		_impl->rootVisual.Size({ static_cast<float>(a_width), static_cast<float>(a_height) });
	}
	if (_impl->controller) {
		_impl->controller->put_Bounds(
			RECT{ 0, 0, static_cast<LONG>(a_width), static_cast<LONG>(a_height) });
	}
	if (_impl->framePool) {
		try {
			std::scoped_lock lock(_impl->captureMutex);
			for (auto& texture : _impl->staging) {
				texture.Reset();
			}
			_impl->stagingWidth = 0;
			_impl->stagingHeight = 0;
			_impl->stagedFrameCount = 0;
			_impl->framePool.Recreate(_impl->captureDevice,
				DirectXPixelFormat::B8G8R8A8UIntNormalized, 3,
				SizeInt32{ static_cast<int32_t>(a_width), static_cast<int32_t>(a_height) });
		} catch (const hresult_error& error) {
			Log(std::format(L"osfui-webview2-poc: capture resize failed: {}", error.message().c_str()));
		}
	}
}

void WebViewCapture::SetVisible(bool a_visible)
{
	_impl->visible = a_visible;
	if (_impl->rootVisual) {
		_impl->rootVisual.IsVisible(a_visible);
	}
	if (_impl->controller) {
		_impl->controller->put_IsVisible(a_visible ? TRUE : FALSE);
	}
}

void WebViewCapture::FocusWebView(bool a_allowWidgetFallback)
{
	if (!_impl->controller || !_impl->visible) {
		return;
	}
	const auto hr = _impl->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
	if (FAILED(hr)) {
		LogHr(L"MoveFocus(PROGRAMMATIC)", hr);
	}
	if (a_allowWidgetFallback && !IsChromeWidget(::GetFocus())) {
		HWND widget = FindChromeWidget(_impl->hostWindow);
		if (!widget) {
			widget = FindChromeWidget(_impl->topLevel);
		}
		if (widget) {
			::SetFocus(widget);
			Log(L"osfui-webview2-poc: explicit Chrome_WidgetWin_* SetFocus fallback used");
		} else {
			Log(L"osfui-webview2-poc: Chrome_WidgetWin_* fallback target not found");
		}
	}
}

void WebViewCapture::RestoreTopLevelFocus()
{
	if (_impl->topLevel) {
		::SetFocus(_impl->topLevel);
	}
}

void WebViewCapture::RunFocusCycleTest()
{
	std::uint32_t passed = 0;
	for (std::uint32_t cycle = 0; cycle < 20; ++cycle) {
		FocusWebView(false);
		const bool topActive = ::GetActiveWindow() == _impl->topLevel;
		const bool browserFocused = IsChromeWidget(::GetFocus());
		RestoreTopLevelFocus();
		const bool restored = ::GetFocus() == _impl->topLevel;
		if (topActive && browserFocused && restored) {
			++passed;
		}
	}
	_impl->focusCyclesPassed.store(passed);
	Log(std::format(L"osfui-webview2-poc: focus cycle test {}/20 passed", passed));
}

void WebViewCapture::NavigateSettings()
{
	_impl->NavigateSettings();
}

void WebViewCapture::NavigateMissing()
{
	if (_impl->webView) {
		_impl->domReady.store(false);
		_impl->webView->Navigate(L"https://osfui.local/__deliberate_404__/missing.html");
	}
}

void WebViewCapture::SendBridgeProbe()
{
	if (!_impl->webView) {
		return;
	}
	const auto sent = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto message = std::format(
		L"{{\"type\":\"poc.nativeProbe\",\"payload\":{{\"ticks\":{}}}}}", sent);
	_impl->webView->PostWebMessageAsString(message.c_str());
	_impl->bridgeLatencyMs.store(-1.0);
	_impl->webView->ExecuteScript(
		LR"((() => {
			const started = performance.now();
			const onMessage = (event) => {
				let message;
				try { message = typeof event.data === 'string' ? JSON.parse(event.data) : event.data; } catch { return; }
				if (message && message.requestId === 'gate-c-probe') {
					chrome.webview.removeEventListener('message', onMessage);
					chrome.webview.postMessage(JSON.stringify({
						type:'poc.bridgeAck', latencyMs:performance.now() - started
					}));
				}
			};
			chrome.webview.addEventListener('message', onMessage);
			window.osfui.postMessage(JSON.stringify({
				type:'ui.command', requestId:'gate-c-probe', payload:{command:'poc.roundTrip'}
			}));
			return 'bridge-probe-sent';
		})())",
		Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
			[](HRESULT a_result, LPCWSTR a_json) -> HRESULT {
				Log(std::format(L"osfui-webview2-poc: bridge probe execute hr=0x{:08X} result={}",
					static_cast<unsigned>(a_result), a_json ? a_json : L"<null>"));
				return S_OK;
			}).Get());
}

void WebViewCapture::ExecuteScriptProbe()
{
	if (!_impl->webView) {
		return;
	}
	auto* impl = _impl.get();
	impl->scriptProbeOk.store(false);
	_impl->webView->ExecuteScript(
		L"console.info('OSF UI Gate C console probe'); "
		L"JSON.stringify({ready:document.readyState,href:location.href,dpr:devicePixelRatio})",
		Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
			[impl](HRESULT a_result, LPCWSTR a_json) -> HRESULT {
				impl->scriptProbeOk.store(SUCCEEDED(a_result) && a_json);
				Log(std::format(L"osfui-webview2-poc: ExecuteScript hr=0x{:08X} result={}",
					static_cast<unsigned>(a_result), a_json ? a_json : L"<null>"));
				return S_OK;
			}).Get());
}

void WebViewCapture::SendMouse(UINT a_message, WPARAM a_wparam, LPARAM a_lparam)
{
	if (!_impl->compositionController) {
		return;
	}

	COREWEBVIEW2_MOUSE_EVENT_KIND kind{};
	UINT32 mouseData = 0;
	switch (a_message) {
	case WM_MOUSEMOVE:
		kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE;
		break;
	case WM_LBUTTONDOWN:
		kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN;
		break;
	case WM_LBUTTONUP:
		kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
		break;
	case WM_MBUTTONDOWN:
		kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN;
		break;
	case WM_MBUTTONUP:
		kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP;
		break;
	case WM_RBUTTONDOWN:
		kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN;
		break;
	case WM_RBUTTONUP:
		kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP;
		break;
	case WM_MOUSEWHEEL:
		kind = COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL;
		mouseData = static_cast<UINT32>(static_cast<std::int32_t>(GET_WHEEL_DELTA_WPARAM(a_wparam)));
		break;
	case WM_MOUSEHWHEEL:
		kind = COREWEBVIEW2_MOUSE_EVENT_KIND_HORIZONTAL_WHEEL;
		mouseData = static_cast<UINT32>(static_cast<std::int32_t>(GET_WHEEL_DELTA_WPARAM(a_wparam)));
		break;
	default:
		return;
	}

	POINT point{ GET_X_LPARAM(a_lparam), GET_Y_LPARAM(a_lparam) };
	if (a_message == WM_MOUSEWHEEL || a_message == WM_MOUSEHWHEEL) {
		::ScreenToClient(_impl->topLevel, &point);
	}
	const auto keys = static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(
		GET_KEYSTATE_WPARAM(a_wparam) &
		(MK_CONTROL | MK_SHIFT | MK_LBUTTON | MK_MBUTTON | MK_RBUTTON | MK_XBUTTON1 | MK_XBUTTON2));
	const auto hr = _impl->compositionController->SendMouseInput(kind, keys, mouseData, point);
	if (FAILED(hr)) {
		LogHr(L"SendMouseInput", hr);
	}
}

bool WebViewCapture::CopyLatestFrame(std::uint64_t& a_lastSerial, Frame& a_frame)
{
	std::scoped_lock lock(_impl->frameMutex);
	if (_impl->latest.serial == 0 || _impl->latest.serial == a_lastSerial) {
		return false;
	}
	a_frame = _impl->latest;
	a_lastSerial = a_frame.serial;
	return true;
}

WebViewCapture::Stats WebViewCapture::GetStats() const
{
	Stats stats;
	stats.captureFrames = _impl->captureFrames.load();
	stats.lastReadbackMs = _impl->lastReadbackMs.load();
	const auto measuredFrames = _impl->measuredFrames.load();
	stats.averageReadbackMs = measuredFrames ?
		_impl->totalReadbackMs.load() / static_cast<double>(measuredFrames) : 0.0;
	stats.webMessages = _impl->webMessages.load();
	stats.consoleMessages = _impl->consoleMessages.load();
	stats.focusCyclesPassed = _impl->focusCyclesPassed.load();
	stats.ready = _impl->ready.load();
	stats.acceleratorEvents = _impl->acceleratorEvents.load();
	stats.focusedValue = _impl->focusedValue;
	stats.bridgeLatencyMs = _impl->bridgeLatencyMs.load();
	stats.lastNavigationSuccess = _impl->lastNavigationSuccess.load();
	stats.lastNavigationError = _impl->lastNavigationError.load();
	stats.translucentPixels = _impl->translucentPixels.load();
	stats.premultiplyViolations = _impl->premultiplyViolations.load();
	stats.lastMissingHttpStatus = _impl->lastMissingHttpStatus.load();
	stats.cursorId = _impl->cursorId.load();
	stats.scriptProbeOk = _impl->scriptProbeOk.load();
	stats.domReady = _impl->domReady.load();
	stats.topLevelActive = ::GetActiveWindow() == _impl->topLevel;
	stats.chromeFocused = IsChromeWidget(::GetFocus());
	return stats;
}

bool WebViewCapture::IsReady() const
{
	return _impl->ready.load();
}

bool WebViewCapture::IsVisible() const
{
	return _impl->visible;
}
