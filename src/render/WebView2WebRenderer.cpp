#include "render/WebView2WebRenderer.h"

#if defined(OSFUI_WITH_WEBVIEW2)

#include <array>
#include <chrono>
#include <deque>
#include <unordered_set>

#include "core/Log.h"
#include "input/OverlayInputHook.h"

#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>
#include <DispatcherQueue.h>
#include <ShlObj.h>
#include <WebView2.h>
#include <wrl.h>
#include <wrl/client.h>
#include <d3d10_1.h>
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
#include <nlohmann/json.hpp>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace OSFUI
{
	namespace
	{
		std::wstring ToWide(std::string_view a_text)
		{
			if (a_text.empty()) return {};
			const auto size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
				a_text.data(), static_cast<int>(a_text.size()), nullptr, 0);
			if (size <= 0) return {};
			std::wstring out(static_cast<std::size_t>(size), L'\0');
			::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_text.data(),
				static_cast<int>(a_text.size()), out.data(), size);
			return out;
		}

		std::string ToUtf8(std::wstring_view a_text)
		{
			if (a_text.empty()) return {};
			const auto size = ::WideCharToMultiByte(CP_UTF8, 0, a_text.data(),
				static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr);
			if (size <= 0) return {};
			std::string out(static_cast<std::size_t>(size), '\0');
			::WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()),
				out.data(), size, nullptr, nullptr);
			return out;
		}

		std::filesystem::path UserDataFolder()
		{
			PWSTR value = nullptr;
			if (FAILED(::SHGetKnownFolderPath(
				FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &value))) {
				return std::filesystem::temp_directory_path() / "OSFUI" / "WebView2";
			}
			std::filesystem::path out(value);
			::CoTaskMemFree(value);
			return out / "OSFUI" / "WebView2";
		}

		struct FindWindowData { DWORD pid; HWND result; };
		BOOL CALLBACK FindWindowProc(HWND a_hwnd, LPARAM a_param)
		{
			auto* data = reinterpret_cast<FindWindowData*>(a_param);
			DWORD pid = 0;
			::GetWindowThreadProcessId(a_hwnd, &pid);
			if (pid == data->pid && ::IsWindowVisible(a_hwnd) &&
				::GetWindow(a_hwnd, GW_OWNER) == nullptr) {
				data->result = a_hwnd;
				return FALSE;
			}
			return TRUE;
		}
		HWND FindTopLevelWindow()
		{
			FindWindowData data{ ::GetCurrentProcessId(), nullptr };
			::EnumWindows(&FindWindowProc, reinterpret_cast<LPARAM>(&data));
			return data.result;
		}

		HWND FindChromeWidget(HWND a_parent)
		{
			struct Search { HWND result; } search{};
			::EnumChildWindows(a_parent, [](HWND a_hwnd, LPARAM a_param) -> BOOL {
				auto* state = reinterpret_cast<Search*>(a_param);
				wchar_t name[128]{};
				::GetClassNameW(a_hwnd, name, static_cast<int>(std::size(name)));
				if (std::wstring_view(name).starts_with(L"Chrome_WidgetWin_")) {
					state->result = a_hwnd;
					return FALSE;
				}
				return TRUE;
			}, reinterpret_cast<LPARAM>(&search));
			return search.result;
		}

		CursorShape CursorFromId(UINT32 a_id)
		{
			switch (a_id) {
			case 0: return CursorShape::kNone;
			case 32513: return CursorShape::kIBeam;
			case 32514: return CursorShape::kCross;
			case 32515: return CursorShape::kWait;
			case 32642: return CursorShape::kSizeNWSE;
			case 32643: return CursorShape::kSizeNESW;
			case 32644: return CursorShape::kSizeAll;
			case 32645: return CursorShape::kSizeWE;
			case 32646: return CursorShape::kSizeNS;
			case 32648: return CursorShape::kNotAllowed;
			case 32649: return CursorShape::kHand;
			case 32651: return CursorShape::kHelp;
			default: return CursorShape::kArrow;
			}
		}

		void LogHr(std::string_view a_where, HRESULT a_hr)
		{
			REX::ERROR("WebView2WebRenderer: {} failed (0x{:08X})",
				a_where, static_cast<unsigned>(a_hr));
		}
	}

	struct WebView2WebRenderer::Impl
	{
		struct Frame {
			std::vector<std::uint8_t> pixels;
			std::uint32_t width{}, height{}, stride{};
			std::uint64_t serial{};
		};
		struct Mouse {
			enum class Kind { Move, Button, Wheel };
			Kind kind{ Kind::Move };
			int x{}, y{}, button{}, wheel{};
			bool down{};
		};
		struct Eval {
			std::string script;
			ScriptResultHandler callback;
		};
		struct Notify {
			enum class Kind { Web, Dom, Load, Eval, Console };
			Kind kind{ Kind::Web };
			std::string text, detail;
			bool failed{};
			int code{};
			ScriptResultHandler callback;
		};

		RendererConfig config;
		std::filesystem::path viewsRoot, userData, mappedViewsRoot;
		std::string viewId;
		bool bridgeAllowed{};

		WebMessageHandler onWebMessage;
		DomReadyHandler onDomReady;
		LoadHandler onLoad;
		CursorChangeHandler onCursorChange;
		NativeAcceleratorHandler onAccelerator;
		ConsoleHandler onConsole;
		std::unordered_map<std::string, JsListenerHandler> listeners;

		std::mutex commandMutex;
		std::optional<ViewManifest> pendingManifest;
		bool loadDirty{};
		std::optional<std::pair<std::uint32_t, std::uint32_t>> pendingResize;
		std::optional<bool> pendingHidden, pendingFocus;
		bool destroyRequested{};
		std::deque<std::string> toWeb;
		std::deque<Mouse> mouse;
		std::deque<Eval> evals;

		std::mutex notifyMutex;
		std::deque<Notify> notifications;
		std::mutex frameMutex;
		Frame latest, renderFrame, scratch;

		std::atomic_bool started{}, stopRequested{}, captureClosing{ true };
		std::thread worker;
		HANDLE wakeEvent{};
		HWND topLevel{}, bootstrapWindow{}, hostWindow{};
		std::uint32_t width{ 1 }, height{ 1 };
		bool hidden{ true }, domSeen{}, navigationSucceeded{}, domNotified{};
		std::wstring currentUrl;

		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		std::array<ComPtr<ID3D11Texture2D>, 3> staging;
		std::uint32_t stagingWrite{}, stagedFrames{}, stagingWidth{}, stagingHeight{};
		double statReadbackMs{};
		std::uint64_t statFrames{};
		std::mutex captureMutex;

		winrt::Windows::System::DispatcherQueueController dispatcher{ nullptr };
		winrt::Windows::UI::Composition::Compositor compositor{ nullptr };
		winrt::Windows::UI::Composition::ContainerVisual rootVisual{ nullptr };
		winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice captureDevice{ nullptr };
		winrt::Windows::Graphics::Capture::GraphicsCaptureItem captureItem{ nullptr };
		winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool{ nullptr };
		winrt::Windows::Graphics::Capture::GraphicsCaptureSession captureSession{ nullptr };
		winrt::event_token frameToken{};

		ComPtr<ICoreWebView2Environment> environment;
		ComPtr<ICoreWebView2Controller> controller;
		ComPtr<ICoreWebView2CompositionController> compositionController;
		ComPtr<ICoreWebView2> webView;
		ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> consoleReceiver;
		std::unordered_set<UINT> handledKeys;
		EventRegistrationToken acceleratorToken{}, cursorToken{}, messageToken{};
		EventRegistrationToken navigationToken{}, domToken{}, processFailedToken{}, consoleToken{};

		void Wake() const { if (wakeEvent) ::SetEvent(wakeEvent); }
		void Push(Notify a_value)
		{
			std::scoped_lock lock(notifyMutex);
			notifications.push_back(std::move(a_value));
		}
		bool Start()
		{
			if (started.exchange(true)) return true;
			wakeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!wakeEvent) {
				REX::ERROR("WebView2WebRenderer: CreateEvent failed ({})", ::GetLastError());
				started.store(false);
				return false;
			}
			REX::INFO("WebView2WebRenderer: starting STA worker on first captured open");
			worker = std::thread([this] { WorkerMain(); });
			return true;
		}

		bool InitializeGraphics()
		{
			const D3D_FEATURE_LEVEL levels[] = {
				D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
			};
			D3D_FEATURE_LEVEL actual{};
			auto hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
				D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
				static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
				&device, &actual, &context);
			if (FAILED(hr)) {
				LogHr("D3D11CreateDevice", hr);
				return false;
			}
			ComPtr<ID3D10Multithread> multithread;
			if (SUCCEEDED(context.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
			ComPtr<IDXGIDevice> dxgi;
			if (FAILED(device.As(&dxgi))) return false;
			winrt::com_ptr<::IInspectable> inspectable;
			hr = ::CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put());
			if (FAILED(hr)) {
				LogHr("CreateDirect3D11DeviceFromDXGIDevice", hr);
				return false;
			}
			captureDevice = inspectable.as<
				winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
			return true;
		}

		bool InitializeComposition()
		{
			DispatcherQueueOptions options{
				sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA
			};
			const auto hr = ::CreateDispatcherQueueController(options,
				reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(
					winrt::put_abi(dispatcher)));
			if (FAILED(hr)) {
				LogHr("CreateDispatcherQueueController", hr);
				return false;
			}
			compositor = winrt::Windows::UI::Composition::Compositor();
			rootVisual = compositor.CreateContainerVisual();
			rootVisual.Size({ static_cast<float>(width), static_cast<float>(height) });
			rootVisual.IsVisible(false);
			return true;
		}

		// Do NOT try to self-register msedgewebview2.exe in the USVFS executable
		// blacklist from here. usvfs_x64.dll does export usvfsBlacklistExecutable
		// (single LPCWSTR arg), but it operates on a process-global
		// SharedParameters pointer that only Mod Organizer 2's own process
		// populates when it builds the VFS. Calling it from inside a hooked game
		// process faults, and the fault leaves USVFS path resolution dead for the
		// rest of the process: every virtual path (including the address library
		// that CommonLibSF needs) stops resolving. Tried 2026-07-19, reverted.
		void ResolveMappedViewsRoot()
		{
			mappedViewsRoot = viewsRoot;
			if (!::GetModuleHandleW(L"usvfs_x64.dll")) return;
			// Mod Organizer 2 presents data/OSFUI/views only inside
			// USVFS-hooked processes. msedgewebview2.exe must stay unhooked
			// (injection crashes it), so the browser serving osfui.local
			// cannot resolve the virtual path. Mirror the tree through this
			// hooked process into a real folder the browser can read.
			std::error_code ec;
			const auto mirror = userData.parent_path() / "views-mirror";
			std::filesystem::remove_all(mirror, ec);
			ec.clear();
			std::filesystem::create_directories(mirror.parent_path(), ec);
			ec.clear();
			std::filesystem::copy(viewsRoot, mirror,
				std::filesystem::copy_options::recursive |
					std::filesystem::copy_options::overwrite_existing, ec);
			if (ec) {
				REX::WARN(
					"WebView2WebRenderer: USVFS detected but views mirror copy "
					"failed ({}); mapping the direct path, which the browser "
					"process may not see", ec.message());
				mappedViewsRoot = viewsRoot;
				return;
			}
			mappedViewsRoot = mirror;
			REX::INFO("WebView2WebRenderer: USVFS detected — views mirrored to {}",
				ToUtf8(mirror.native()));
		}

		bool BeginEnvironment()
		{
			std::error_code ec;
			std::filesystem::create_directories(userData, ec);
			const auto callback =
				Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
					[this](HRESULT a_hr, ICoreWebView2Environment* a_environment) -> HRESULT {
						if (stopRequested.load()) return S_OK;
						if (FAILED(a_hr) || !a_environment) {
							LogHr("environment callback", a_hr);
							return S_OK;
						}
						environment = a_environment;
						ComPtr<ICoreWebView2Environment3> environment3;
						if (FAILED(environment.As(&environment3))) {
							REX::ERROR("WebView2WebRenderer: composition API unavailable");
							return S_OK;
						}
						const auto result =
							environment3->CreateCoreWebView2CompositionController(hostWindow,
								Callback<
									ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
									[this](HRESULT a_controllerHr,
										ICoreWebView2CompositionController* a_controller) -> HRESULT {
										return OnController(a_controllerHr, a_controller);
									}).Get());
						if (FAILED(result)) LogHr("CreateCompositionController", result);
						return S_OK;
					});
			const auto hr = ::CreateCoreWebView2EnvironmentWithOptions(
				nullptr, userData.c_str(), nullptr, callback.Get());
			if (FAILED(hr)) {
				LogHr("CreateCoreWebView2EnvironmentWithOptions", hr);
				return false;
			}
			return true;
		}

		HRESULT OnController(
			HRESULT a_hr, ICoreWebView2CompositionController* a_composition)
		{
			if (stopRequested.load()) return S_OK;
			if (FAILED(a_hr) || !a_composition) {
				LogHr("composition controller callback", a_hr);
				if (a_hr == E_UNEXPECTED) {
					// Mod Organizer 2's USVFS injects into every child process the
					// game spawns and crashes msedgewebview2.exe on launch, which
					// surfaces here as E_UNEXPECTED.
					REX::ERROR(
						"WebView2WebRenderer: the WebView2 browser process likely "
						"crashed on launch. If running under Mod Organizer 2, add "
						"msedgewebview2.exe to Settings > Workarounds > "
						"Executables Blacklist and relaunch the game");
				}
				return S_OK;
			}
			compositionController = a_composition;

			// WebView2 requires the controller's parent HWND to belong to the
			// controller STA. Starfield's top-level window belongs to its UI thread,
			// so the worker creates the controller against a worker-owned popup first.
			// Once Chromium exists, make that HWND a real child of the game window so
			// normal Win32 focus and IME routing still work while the overlay is open.
			::SetLastError(ERROR_SUCCESS);
			const auto oldParent = ::SetParent(hostWindow, topLevel);
			const auto parentError = ::GetLastError();
			if (!oldParent && parentError != ERROR_SUCCESS) {
				REX::ERROR("WebView2WebRenderer: host reparent failed ({})", parentError);
				return S_OK;
			}
			const auto style = static_cast<DWORD_PTR>(
				::GetWindowLongPtrW(hostWindow, GWL_STYLE));
			::SetWindowLongPtrW(hostWindow, GWL_STYLE,
				static_cast<LONG_PTR>((style & ~WS_POPUP) | WS_CHILD | WS_VISIBLE));
			::SetWindowPos(hostWindow, nullptr, 0, 0, 1, 1,
				SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_FRAMECHANGED);
			REX::INFO("WebView2WebRenderer: worker-owned host reparented beneath game HWND");
			if (bootstrapWindow) {
				::DestroyWindow(bootstrapWindow);
				bootstrapWindow = nullptr;
			}

			if (FAILED(compositionController.As(&controller)) ||
				FAILED(controller->get_CoreWebView2(&webView)) || !webView) {
				REX::ERROR("WebView2WebRenderer: failed to acquire CoreWebView2");
				return S_OK;
			}
			controller->put_Bounds(
				RECT{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) });
			// The runtime's unhide command may already have been drained before
			// the controller existed, so apply the current state instead of a
			// hardcoded FALSE — an invisible controller suspends Chromium
			// rendering and the capture pipeline never sees a painted frame.
			controller->put_IsVisible(hidden ? FALSE : TRUE);
			ComPtr<ICoreWebView2Controller2> controller2;
			if (SUCCEEDED(controller.As(&controller2))) {
				controller2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{ 0, 0, 0, 0 });
			}
			const auto root = rootVisual.as<::IUnknown>();
			auto result = compositionController->put_RootVisualTarget(root.get());
			if (FAILED(result)) {
				LogHr("put_RootVisualTarget", result);
				return S_OK;
			}
			ComPtr<ICoreWebView2_3> webView3;
			if (FAILED(webView.As(&webView3))) {
				REX::ERROR("WebView2WebRenderer: virtual host mapping API unavailable");
				return S_OK;
			}
			result = webView3->SetVirtualHostNameToFolderMapping(
				L"osfui.local", mappedViewsRoot.c_str(),
				COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
			if (FAILED(result)) {
				LogHr("SetVirtualHostNameToFolderMapping", result);
				return S_OK;
			}
			InstallEvents();
			if (bridgeAllowed) {
				static constexpr wchar_t shim[] = LR"JS(
					(() => {
						const bridge = window.osfui = window.osfui || {};
						const pending = [];
						let onMessage = typeof bridge.onMessage === 'function' ?
							bridge.onMessage : null;
						Object.defineProperty(bridge, 'onMessage', {
							configurable: true,
							get: () => onMessage,
							set: (fn) => {
								onMessage = fn;
								if (typeof fn === 'function')
									pending.splice(0).forEach((json) => fn(json));
							}
						});
						bridge.postMessage = (json) => chrome.webview.postMessage(String(json));
						bridge.__invokeListener = (name, arg) =>
							chrome.webview.postMessage(JSON.stringify({
								__osfuiListener: String(name), argument: String(arg)
							}));
						chrome.webview.addEventListener('message', (event) => {
							const json = typeof event.data === 'string' ?
								event.data : JSON.stringify(event.data);
							if (typeof onMessage === 'function') onMessage(json);
							else pending.push(json);
						});
					})();)JS";
				webView->AddScriptToExecuteOnDocumentCreated(shim,
					Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
						[](HRESULT a_scriptHr, LPCWSTR) -> HRESULT {
							if (FAILED(a_scriptHr)) LogHr("bridge shim", a_scriptHr);
							return S_OK;
						}).Get());
			}
			if (!StartCapture()) return S_OK;
			REX::INFO("WebView2WebRenderer: composition controller and capture ready");
			DrainCommands();
			return S_OK;
		}

		void InstallEvents()
		{
			compositionController->add_CursorChanged(
				Callback<ICoreWebView2CursorChangedEventHandler>(
					[this](ICoreWebView2CompositionController* a_sender, ::IUnknown*) -> HRESULT {
						UINT32 id = 0;
						if (SUCCEEDED(a_sender->get_SystemCursorId(&id)) && onCursorChange)
							onCursorChange(CursorFromId(id));
						return S_OK;
					}).Get(), &cursorToken);
			controller->add_AcceleratorKeyPressed(
				Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
					[this](ICoreWebView2Controller*,
						ICoreWebView2AcceleratorKeyPressedEventArgs* a_args) -> HRESULT {
						UINT key = 0;
						COREWEBVIEW2_KEY_EVENT_KIND kind{};
						a_args->get_VirtualKey(&key);
						a_args->get_KeyEventKind(&kind);
						const bool down =
							kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
							kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN;
						bool handled = down && handledKeys.contains(key);
						if (!handled && onAccelerator) handled = onAccelerator(key, down);
						if (handled) {
							a_args->put_Handled(TRUE);
							if (down) handledKeys.insert(key);
						}
						if (!down) handledKeys.erase(key);
						return S_OK;
					}).Get(), &acceleratorToken);
			webView->add_WebMessageReceived(
				Callback<ICoreWebView2WebMessageReceivedEventHandler>(
					[this](ICoreWebView2*,
						ICoreWebView2WebMessageReceivedEventArgs* a_args) -> HRESULT {
						if (!bridgeAllowed) return S_OK;
						LPWSTR value = nullptr;
						if (FAILED(a_args->TryGetWebMessageAsString(&value)) || !value)
							return S_OK;
						auto text = ToUtf8(value);
						::CoTaskMemFree(value);
						Push(Notify{ .kind = Notify::Kind::Web, .text = std::move(text) });
						return S_OK;
					}).Get(), &messageToken);
			webView->add_NavigationCompleted(
				Callback<ICoreWebView2NavigationCompletedEventHandler>(
					[this](ICoreWebView2*,
						ICoreWebView2NavigationCompletedEventArgs* a_args) -> HRESULT {
						BOOL success = FALSE;
						COREWEBVIEW2_WEB_ERROR_STATUS status{};
						a_args->get_IsSuccess(&success);
						a_args->get_WebErrorStatus(&status);
						Push(Notify{
							.kind = Notify::Kind::Load,
							.text = ToUtf8(currentUrl),
							.detail = success ? "" : "WebView2 navigation failed",
							.failed = success != TRUE,
							.code = static_cast<int>(status)
						});
						navigationSucceeded = success == TRUE;
						if (navigationSucceeded && domSeen && !domNotified) {
							domNotified = true;
							Push(Notify{ .kind = Notify::Kind::Dom });
						}
						return S_OK;
					}).Get(), &navigationToken);
			ComPtr<ICoreWebView2_2> webView2;
			if (SUCCEEDED(webView.As(&webView2))) {
				webView2->add_DOMContentLoaded(
					Callback<ICoreWebView2DOMContentLoadedEventHandler>(
						[this](ICoreWebView2*,
							ICoreWebView2DOMContentLoadedEventArgs*) -> HRESULT {
							domSeen = true;
							if (navigationSucceeded && !domNotified) {
								domNotified = true;
								Push(Notify{ .kind = Notify::Kind::Dom });
							}
							DrainCommands();
							return S_OK;
						}).Get(), &domToken);
			}
			webView->add_ProcessFailed(
				Callback<ICoreWebView2ProcessFailedEventHandler>(
					[](ICoreWebView2*,
						ICoreWebView2ProcessFailedEventArgs* a_args) -> HRESULT {
						COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
						a_args->get_ProcessFailedKind(&kind);
						REX::ERROR("WebView2WebRenderer: browser process failed (kind {})",
							static_cast<int>(kind));
						return S_OK;
					}).Get(), &processFailedToken);
			if (SUCCEEDED(webView->GetDevToolsProtocolEventReceiver(
				L"Runtime.consoleAPICalled", &consoleReceiver)) && consoleReceiver) {
				consoleReceiver->add_DevToolsProtocolEventReceived(
					Callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
						[this](ICoreWebView2*,
							ICoreWebView2DevToolsProtocolEventReceivedEventArgs* a_args) -> HRESULT {
							LPWSTR value = nullptr;
							if (SUCCEEDED(a_args->get_ParameterObjectAsJson(&value)) && value) {
								Push(Notify{ .kind = Notify::Kind::Console,
									.text = ToUtf8(value) });
								::CoTaskMemFree(value);
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
			captureClosing.store(false);
			try {
				using namespace winrt::Windows::Graphics;
				using namespace winrt::Windows::Graphics::Capture;
				using namespace winrt::Windows::Graphics::DirectX;
				captureItem = GraphicsCaptureItem::CreateFromVisual(rootVisual);
				framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
					captureDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 3,
					SizeInt32{ static_cast<std::int32_t>(width),
						static_cast<std::int32_t>(height) });
				frameToken = framePool.FrameArrived(
					[this](Direct3D11CaptureFramePool const& a_pool,
						winrt::Windows::Foundation::IInspectable const&) {
						OnFrameArrived(a_pool);
					});
				captureSession = framePool.CreateCaptureSession(captureItem);
				try { captureSession.IsCursorCaptureEnabled(false); } catch (...) {}
				captureSession.StartCapture();
				return true;
			} catch (const winrt::hresult_error& a_error) {
				REX::ERROR("WebView2WebRenderer: capture setup failed: {}",
					ToUtf8(a_error.message()));
				return false;
			}
		}

		void OnFrameArrived(
			const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& a_pool)
		{
			if (stopRequested.load() || captureClosing.load()) return;
			try {
				const auto readbackStart = std::chrono::steady_clock::now();
				auto captured = a_pool.TryGetNextFrame();
				if (!captured) return;
				auto access = captured.Surface().as<
					::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
				ComPtr<ID3D11Texture2D> source;
				winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&source)));
				D3D11_TEXTURE2D_DESC desc{};
				source->GetDesc(&desc);

				std::scoped_lock captureLock(captureMutex);
				if (captureClosing.load()) return;
				if (!staging[0] || desc.Width != stagingWidth ||
					desc.Height != stagingHeight) {
					auto stagingDesc = desc;
					stagingDesc.BindFlags = 0;
					stagingDesc.MiscFlags = 0;
					stagingDesc.Usage = D3D11_USAGE_STAGING;
					stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					for (auto& texture : staging) {
						texture.Reset();
						winrt::check_hresult(device->CreateTexture2D(
							&stagingDesc, nullptr, &texture));
					}
					stagingWidth = desc.Width;
					stagingHeight = desc.Height;
					stagingWrite = stagedFrames = 0;
				}
				context->CopyResource(staging[stagingWrite].Get(), source.Get());
				// Map the slot just written: on the dedicated capture device this
				// waits only for the copy above (~3 ms at 1080p per the POC), and
				// unlike the oldest-slot ring it adds no captured-frames of
				// interaction latency and never drops a burst that ends early.
				const auto readIndex = stagingWrite;
				stagingWrite = (stagingWrite + 1) % staging.size();
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (FAILED(context->Map(
					staging[readIndex].Get(), 0, D3D11_MAP_READ, 0, &mapped)))
					return;
				scratch.width = desc.Width;
				scratch.height = desc.Height;
				scratch.stride = desc.Width * 4;
				scratch.pixels.resize(
					static_cast<std::size_t>(scratch.stride) * scratch.height);
				const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData);
				for (std::uint32_t row = 0; row < scratch.height; ++row) {
					std::memcpy(
						scratch.pixels.data() +
							static_cast<std::size_t>(row) * scratch.stride,
						bytes + static_cast<std::size_t>(row) * mapped.RowPitch,
						scratch.stride);
				}
				context->Unmap(staging[readIndex].Get(), 0);
				++scratch.serial;
				if (scratch.serial == 1)
					REX::INFO("WebView2WebRenderer: first capture frame published "
						"({}x{})", scratch.width, scratch.height);
				statReadbackMs += std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - readbackStart).count();
				++statFrames;
				if (statFrames == 120 || statFrames % 1800 == 0) {
					REX::INFO("WebView2WebRenderer: capture stats — {} frames, "
						"avg readback {:.2f} ms ({}x{})",
						statFrames, statReadbackMs / statFrames,
						scratch.width, scratch.height);
				}
				std::scoped_lock frameLock(frameMutex);
				latest.pixels.swap(scratch.pixels);
				latest.width = scratch.width;
				latest.height = scratch.height;
				latest.stride = scratch.stride;
				latest.serial = scratch.serial;
			} catch (const winrt::hresult_error& a_error) {
				REX::WARN("WebView2WebRenderer: capture callback failed: {}",
					ToUtf8(a_error.message()));
			}
		}

		void ApplyResize(std::uint32_t a_width, std::uint32_t a_height)
		{
			width = (std::max)(1u, a_width);
			height = (std::max)(1u, a_height);
			if (rootVisual)
				rootVisual.Size({ static_cast<float>(width), static_cast<float>(height) });
			if (controller)
				controller->put_Bounds(
					RECT{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) });
			if (!framePool) return;
			try {
				std::scoped_lock lock(captureMutex);
				for (auto& texture : staging) texture.Reset();
				stagingWidth = stagingHeight = stagedFrames = 0;
				framePool.Recreate(captureDevice,
					winrt::Windows::Graphics::DirectX::
						DirectXPixelFormat::B8G8R8A8UIntNormalized,
					3, winrt::Windows::Graphics::SizeInt32{
						static_cast<std::int32_t>(width),
						static_cast<std::int32_t>(height) });
			} catch (const winrt::hresult_error& a_error) {
				REX::WARN("WebView2WebRenderer: resize failed: {}",
					ToUtf8(a_error.message()));
			}
		}

		void Navigate(const ViewManifest& a_manifest)
		{
			if (!webView) return;
			domSeen = false;
			navigationSucceeded = false;
			domNotified = false;
			std::string path = a_manifest.id + "/" + a_manifest.entry;
			std::ranges::replace(path, '\\', '/');
			currentUrl = L"https://osfui.local/" + ToWide(path);
			const auto result = webView->Navigate(currentUrl.c_str());
			if (FAILED(result)) {
				Push(Notify{
					.kind = Notify::Kind::Load,
					.text = ToUtf8(currentUrl),
					.detail = "Navigate returned failure",
					.failed = true,
					.code = static_cast<int>(result)
				});
			}
		}

		void SendMouse(const Mouse& a_input)
		{
			if (!compositionController) return;
			COREWEBVIEW2_MOUSE_EVENT_KIND kind{};
			UINT32 data = 0;
			if (a_input.kind == Mouse::Kind::Move) {
				kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE;
			} else if (a_input.kind == Mouse::Kind::Wheel) {
				kind = COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL;
				data = static_cast<UINT32>(static_cast<std::int32_t>(a_input.wheel));
			} else if (a_input.button == 0) {
				kind = a_input.down ? COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN :
					COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
			} else if (a_input.button == 1) {
				kind = a_input.down ? COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN :
					COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP;
			} else {
				kind = a_input.down ? COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN :
					COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP;
			}
			compositionController->SendMouseInput(kind,
				COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, data,
				POINT{ a_input.x, a_input.y });
		}

		void DrainCommands()
		{
			std::optional<ViewManifest> manifest;
			std::optional<std::pair<std::uint32_t, std::uint32_t>> resize;
			std::optional<bool> setHidden, setFocus;
			std::deque<std::string> messages;
			std::deque<Mouse> input;
			std::deque<Eval> scripts;
			bool destroy = false;
			{
				std::scoped_lock lock(commandMutex);
				resize.swap(pendingResize);
				setHidden.swap(pendingHidden);
				if (controller) setFocus.swap(pendingFocus);
				destroy = std::exchange(destroyRequested, false);
				if (webView) {
					if (loadDirty && pendingManifest) {
						manifest = *pendingManifest;
						loadDirty = false;
					}
					if (domSeen) {
						messages.swap(toWeb);
						input.swap(mouse);
						scripts.swap(evals);
					}
				}
			}
			if (resize) ApplyResize(resize->first, resize->second);
			if (setHidden) {
				hidden = *setHidden;
				if (rootVisual) rootVisual.IsVisible(!hidden);
				if (controller) controller->put_IsVisible(hidden ? FALSE : TRUE);
				if (!hidden) {
					// Reveal contract (Runtime::SubmitFrameIfVisible): a (re)shown
					// view republishes under a fresh serial. WGC only delivers
					// frames when content changes, so a static page would
					// otherwise never satisfy the reveal on reopen.
					std::scoped_lock republishLock(captureMutex, frameMutex);
					if (latest.serial != 0 || renderFrame.serial != 0) {
						if (renderFrame.serial == latest.serial) {
							// After Render()'s swap the newest pixels live in
							// renderFrame; restore them before re-serializing.
							latest.pixels = renderFrame.pixels;
							latest.width = renderFrame.width;
							latest.height = renderFrame.height;
							latest.stride = renderFrame.stride;
						}
						latest.serial = ++scratch.serial;
					}
				}
			}
			if (destroy) {
				CloseWebResources();
				return;
			}
			if (manifest) Navigate(*manifest);
			if (webView) {
				for (const auto& message : messages) {
					const auto wide = ToWide(message);
					webView->PostWebMessageAsString(wide.c_str());
				}
				for (const auto& value : input) SendMouse(value);
				for (auto& eval : scripts) {
					const auto script = ToWide(eval.script);
					auto callback = std::move(eval.callback);
					webView->ExecuteScript(script.c_str(),
						Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
							[this, callback = std::move(callback)](
								HRESULT a_hr, LPCWSTR a_json) mutable -> HRESULT {
								if (callback) {
									Push(Notify{
										.kind = Notify::Kind::Eval,
										.text = SUCCEEDED(a_hr) && a_json ?
											ToUtf8(a_json) : std::string{},
										.callback = std::move(callback)
									});
								}
								return S_OK;
							}).Get());
				}
			}
			if (setFocus) {
				if (*setFocus && controller && !hidden) {
					const auto hr = controller->MoveFocus(
						COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
					if (FAILED(hr)) {
						if (const auto chrome = FindChromeWidget(hostWindow)) {
							::SetFocus(chrome);
							REX::WARN("WebView2WebRenderer: used Chrome focus fallback");
						}
					}
				} else if (!*setFocus && topLevel) {
					handledKeys.clear();
					::PostMessageW(topLevel,
						OverlayInputHook::kRestoreGameFocusMessage, 0, 0);
				}
			}
		}

		void CloseWebResources()
		{
			captureClosing.store(true);
			if (framePool) {
				try { framePool.FrameArrived(frameToken); } catch (...) {}
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
			{
				std::scoped_lock lock(captureMutex);
				for (auto& texture : staging) texture.Reset();
			}
			if (compositionController)
				compositionController->put_RootVisualTarget(nullptr);
			if (controller) controller->Close();
			consoleReceiver.Reset();
			webView.Reset();
			compositionController.Reset();
			controller.Reset();
			environment.Reset();
		}

		void WorkerMain()
		{
			try {
				winrt::init_apartment(winrt::apartment_type::single_threaded);
				topLevel = FindTopLevelWindow();
				if (!topLevel) {
					REX::ERROR("WebView2WebRenderer: game top-level HWND not found");
				} else {
					// Match the working POC exactly: a visible child beneath a top-level
					// window, with both HWNDs owned by this controller STA.
					bootstrapWindow = ::CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC",
						L"OSFUI WebView2 Bootstrap", WS_POPUP | WS_VISIBLE,
						-32000, -32000, 1, 1, nullptr, nullptr,
						::GetModuleHandleW(nullptr), nullptr);
					if (!bootstrapWindow) {
						REX::ERROR("WebView2WebRenderer: bootstrap HWND creation failed ({})",
							::GetLastError());
					} else {
						hostWindow = ::CreateWindowExW(0, L"STATIC", L"OSFUI WebView2 Host",
							WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, bootstrapWindow, nullptr,
							::GetModuleHandleW(nullptr), nullptr);
					}
					if (!hostWindow) {
						REX::ERROR("WebView2WebRenderer: child HWND creation failed ({})",
							::GetLastError());
					} else {
						ResolveMappedViewsRoot();
						if (InitializeGraphics() && InitializeComposition() &&
							BeginEnvironment()) {
							while (!stopRequested.load()) {
								const DWORD wait = ::MsgWaitForMultipleObjectsEx(
									1, &wakeEvent, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
								if (wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT ||
									wait == WAIT_OBJECT_0 + 1)
									DrainCommands();
								MSG message{};
								while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
									::TranslateMessage(&message);
									::DispatchMessageW(&message);
								}
							}
						}
					}
				}
			} catch (const winrt::hresult_error& a_error) {
				REX::ERROR("WebView2WebRenderer: worker failed: {} (0x{:08X})",
					ToUtf8(a_error.message()), static_cast<unsigned>(a_error.code()));
			}
			CloseWebResources();
			if (dispatcher) {
				try { dispatcher.ShutdownQueueAsync(); } catch (...) {}
				dispatcher = nullptr;
			}
			rootVisual = nullptr;
			compositor = nullptr;
			captureDevice = nullptr;
			context.Reset();
			device.Reset();
			if (hostWindow) {
				::DestroyWindow(hostWindow);
				hostWindow = nullptr;
			}
			if (bootstrapWindow) {
				::DestroyWindow(bootstrapWindow);
				bootstrapWindow = nullptr;
			}
			REX::INFO("WebView2WebRenderer: STA worker stopped");
		}

		void DrainNotifications()
		{
			std::deque<Notify> local;
			{
				std::scoped_lock lock(notifyMutex);
				local.swap(notifications);
			}
			for (auto& value : local) {
				switch (value.kind) {
				case Notify::Kind::Web:
					try {
						const auto json = nlohmann::json::parse(value.text);
						if (json.contains("__osfuiListener")) {
							const auto name = json.value("__osfuiListener", "");
							const auto found = listeners.find(name);
							if (found != listeners.end() && found->second)
								found->second(json.value("argument", ""));
							break;
						}
					} catch (...) {}
					if (onWebMessage) onWebMessage(viewId, value.text);
					break;
				case Notify::Kind::Dom:
					if (onDomReady) onDomReady(viewId);
					break;
				case Notify::Kind::Load:
					if (onLoad) {
						const LoadEvent event{
							.viewId = viewId,
							.failed = value.failed,
							.url = value.text,
							.description = value.detail,
							.errorDomain = "WebView2",
							.errorCode = value.code
						};
						onLoad(event);
					}
					break;
				case Notify::Kind::Eval:
					if (value.callback) value.callback(std::move(value.text));
					break;
				case Notify::Kind::Console:
					DeliverConsole(value.text);
					break;
				}
			}
		}

		void DeliverConsole(const std::string& a_payload)
		{
			if (!onConsole) return;
			int level = 0;
			std::string text = a_payload;
			try {
				const auto json = nlohmann::json::parse(a_payload);
				const auto type = json.value("type", "log");
				level = type == "warning" ? 1 : type == "error" ? 2 :
					type == "debug" ? 3 : type == "info" ? 4 : 0;
				if (const auto it = json.find("args");
					it != json.end() && it->is_array() && !it->empty()) {
					const auto& first = it->front();
					text = first.value("value",
						first.value("description", a_payload));
				}
			} catch (...) {}
			onConsole(level, std::move(text));
		}
	};

	WebView2WebRenderer::WebView2WebRenderer() :
		_impl(std::make_unique<Impl>())
	{}

	WebView2WebRenderer::~WebView2WebRenderer() { Shutdown(); }

	bool WebView2WebRenderer::RuntimeAvailable()
	{
		wchar_t forced[2]{};
		if (::GetEnvironmentVariableW(
			L"OSFUI_WEBVIEW2_FORCE_RUNTIME_ABSENT", forced, 2) > 0)
			return false;
		LPWSTR version = nullptr;
		const auto result =
			::GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
		if (FAILED(result) || !version) return false;
		REX::INFO("WebView2WebRenderer: evergreen runtime {}", ToUtf8(version));
		::CoTaskMemFree(version);
		return true;
	}

	bool WebView2WebRenderer::Initialize(const RendererConfig& a_config)
	{
		if (!RuntimeAvailable()) return false;
		_impl->config = a_config;
		_impl->viewsRoot = a_config.dataDir / "views";
		_impl->userData = UserDataFolder();
		_impl->width = (std::max)(1u, a_config.width);
		_impl->height = (std::max)(1u, a_config.height);
		return true;
	}

	void WebView2WebRenderer::Shutdown()
	{
		if (!_impl || !_impl->started.load()) return;
		_impl->stopRequested.store(true);
		_impl->Wake();
		if (_impl->worker.joinable()) _impl->worker.join();
		if (_impl->wakeEvent) {
			::CloseHandle(_impl->wakeEvent);
			_impl->wakeEvent = nullptr;
		}
		_impl->started.store(false);
	}

	void WebView2WebRenderer::LoadView(const ViewManifest& a_manifest)
	{
		std::scoped_lock lock(_impl->commandMutex);
		if (!_impl->viewId.empty() && _impl->viewId != a_manifest.id) {
			REX::WARN("WebView2WebRenderer: Phase 2 supports one view; ignoring '{}'",
				a_manifest.id);
			return;
		}
		_impl->viewId = a_manifest.id;
		_impl->bridgeAllowed = a_manifest.permissions.nativeBridge;
		_impl->pendingManifest = a_manifest;
		_impl->loadDirty = true;
		_impl->Wake();
	}

	void WebView2WebRenderer::SetActiveView(std::string_view a_id)
	{
		if (!_impl->viewId.empty() && a_id != _impl->viewId)
			REX::DEBUG("WebView2WebRenderer: ignored active view '{}' (single-view)",
				a_id);
	}

	void WebView2WebRenderer::Resize(
		std::uint32_t a_width, std::uint32_t a_height)
	{
		if (!a_width || !a_height) return;
		{
			std::scoped_lock lock(_impl->commandMutex);
			_impl->pendingResize = std::pair{ a_width, a_height };
		}
		_impl->Wake();
	}

	void WebView2WebRenderer::Update(double)
	{
		// Warm the browser stack once the game loop is ticking (window exists)
		// instead of on the first F10: environment creation, msedgewebview2
		// spawn, navigation, and the output-size resize together take seconds,
		// which otherwise all land inside the first open. The view loads while
		// hidden; opening then only unhides and republishes.
		if (!_impl->started.load()) {
			bool wantsView = false;
			{
				std::scoped_lock lock(_impl->commandMutex);
				wantsView = _impl->pendingManifest.has_value();
			}
			if (wantsView) _impl->Start();
		}
		_impl->DrainNotifications();
	}

	std::optional<FrameBufferView> WebView2WebRenderer::Render()
	{
		std::scoped_lock lock(_impl->frameMutex);
		if (_impl->latest.serial == 0) return std::nullopt;
		if (_impl->renderFrame.serial != _impl->latest.serial) {
			_impl->renderFrame.pixels.swap(_impl->latest.pixels);
			_impl->renderFrame.width = _impl->latest.width;
			_impl->renderFrame.height = _impl->latest.height;
			_impl->renderFrame.stride = _impl->latest.stride;
			_impl->renderFrame.serial = _impl->latest.serial;
		}
		return FrameBufferView{
			.pixels = _impl->renderFrame.pixels,
			.width = _impl->renderFrame.width,
			.height = _impl->renderFrame.height,
			.strideBytes = _impl->renderFrame.stride,
			.format = PixelFormat::kBGRA8,
			.frameIndex = _impl->renderFrame.serial,
			.dirty = DirtyRect::Full(
				_impl->renderFrame.width, _impl->renderFrame.height)
		};
	}

	void WebView2WebRenderer::SendMessageToWeb(
		std::string_view a_viewId, std::string_view a_json)
	{
		if (a_viewId != _impl->viewId || !_impl->bridgeAllowed) return;
		{
			std::scoped_lock lock(_impl->commandMutex);
			_impl->toWeb.emplace_back(a_json);
		}
		_impl->Wake();
	}

	void WebView2WebRenderer::SetWebMessageHandler(WebMessageHandler a_handler)
	{
		_impl->onWebMessage = std::move(a_handler);
	}
	void WebView2WebRenderer::SetDomReadyHandler(DomReadyHandler a_handler)
	{
		_impl->onDomReady = std::move(a_handler);
	}
	void WebView2WebRenderer::SetLoadHandler(LoadHandler a_handler)
	{
		_impl->onLoad = std::move(a_handler);
	}
	void WebView2WebRenderer::SetCursorChangeHandler(CursorChangeHandler a_handler)
	{
		_impl->onCursorChange = std::move(a_handler);
	}
	void WebView2WebRenderer::SetNativeAcceleratorHandler(
		NativeAcceleratorHandler a_handler)
	{
		_impl->onAccelerator = std::move(a_handler);
	}
	void WebView2WebRenderer::SetNativeKeyboardFocus(bool a_focused)
	{
		{
			std::scoped_lock lock(_impl->commandMutex);
			_impl->pendingFocus = a_focused;
		}
		if (a_focused) {
			_impl->Start();
		}
		_impl->Wake();
	}

	void WebView2WebRenderer::InjectMouseMove(int a_x, int a_y)
	{
		{
			std::scoped_lock lock(_impl->commandMutex);
			_impl->mouse.push_back({
				.kind = Impl::Mouse::Kind::Move, .x = a_x, .y = a_y });
		}
		_impl->Wake();
	}
	void WebView2WebRenderer::InjectMouseButton(
		int a_x, int a_y, int a_button, bool a_down)
	{
		{
			std::scoped_lock lock(_impl->commandMutex);
			_impl->mouse.push_back({
				.kind = Impl::Mouse::Kind::Button, .x = a_x, .y = a_y,
				.button = a_button, .down = a_down });
		}
		_impl->Wake();
	}
	void WebView2WebRenderer::InjectMouseWheel(
		int a_x, int a_y, int a_wheelDelta)
	{
		{
			std::scoped_lock lock(_impl->commandMutex);
			_impl->mouse.push_back({
				.kind = Impl::Mouse::Kind::Wheel, .x = a_x, .y = a_y,
				.wheel = a_wheelDelta });
		}
		_impl->Wake();
	}

	void WebView2WebRenderer::EvaluateScript(
		std::string_view a_viewId, std::string_view a_js,
		ScriptResultHandler a_onResult)
	{
		if (a_viewId != _impl->viewId) return;
		{
			std::scoped_lock lock(_impl->commandMutex);
			_impl->evals.push_back({
				.script = std::string(a_js), .callback = std::move(a_onResult) });
		}
		_impl->Wake();
	}

	void WebView2WebRenderer::CallJsFunction(
		std::string_view a_viewId, std::string_view a_fnName,
		std::string_view a_arg)
	{
		const auto name = nlohmann::json(std::string(a_fnName)).dump();
		const auto arg = nlohmann::json(std::string(a_arg)).dump();
		EvaluateScript(a_viewId,
			"typeof window[" + name + "]==='function' ? window[" +
			name + "](" + arg + ") : undefined");
	}

	void WebView2WebRenderer::RegisterJsFunction(
		std::string_view a_viewId, std::string_view a_name,
		JsListenerHandler a_callback)
	{
		if (a_viewId != _impl->viewId) return;
		_impl->listeners[std::string(a_name)] = std::move(a_callback);
		const auto name = nlohmann::json(std::string(a_name)).dump();
		EvaluateScript(a_viewId,
			"window[" + name + "]=function(a){if(window.osfui&&"
			"window.osfui.__invokeListener)window.osfui.__invokeListener(" +
			name + ",a===undefined?'':String(a));};");
	}

	void WebView2WebRenderer::SetConsoleHandler(
		std::string_view a_viewId, ConsoleHandler a_handler)
	{
		if (a_viewId == _impl->viewId)
			_impl->onConsole = std::move(a_handler);
	}

	void WebView2WebRenderer::SetViewHidden(
		std::string_view a_viewId, bool a_hidden)
	{
		if (a_viewId != _impl->viewId) return;
		{
			std::scoped_lock lock(_impl->commandMutex);
			_impl->pendingHidden = a_hidden;
		}
		_impl->Wake();
	}

	void WebView2WebRenderer::DestroyView(std::string_view a_viewId)
	{
		if (a_viewId != _impl->viewId) return;
		{
			std::scoped_lock lock(_impl->commandMutex);
			_impl->destroyRequested = true;
		}
		_impl->Wake();
	}
}

#endif
