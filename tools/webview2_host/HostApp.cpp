#include "HostApp.h"

#include "core/Version.h"
#include "Wv2BrokerLaunch.h"  // LaunchMethodName (logging only)
#include "Wv2Pipe.h"
#include "Wv2Protocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <vector>

#include <DispatcherQueue.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <shellapi.h>
#include <wrl.h>
#include <wrl/client.h>
#include <d3d10_1.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
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
using nlohmann::json;

namespace osfui::wv2
{
	namespace
	{
		std::wstring ToWide(std::string_view a_text)
		{
			if (a_text.empty()) return {};
			const auto size = ::MultiByteToWideChar(CP_UTF8, 0,
				a_text.data(), static_cast<int>(a_text.size()), nullptr, 0);
			if (size <= 0) return {};
			std::wstring out(static_cast<std::size_t>(size), L'\0');
			::MultiByteToWideChar(CP_UTF8, 0, a_text.data(),
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

		struct Logger
		{
			std::ofstream file;
			std::mutex    mutex;
			Pipe*         pipe{ nullptr };  // set once the pipe is up; nulled at teardown

			void Open(const std::filesystem::path& a_path)
			{
				if (a_path.empty()) return;
				std::error_code ec;
				std::filesystem::create_directories(a_path.parent_path(), ec);
				file.open(a_path, std::ios::out | std::ios::trunc);
			}

			// level: 0 info, 1 warn, 2 error
			void Log(int a_level, const std::string& a_text)
			{
				{
					std::scoped_lock lock(mutex);
					if (file.is_open()) {
						const auto now = std::chrono::system_clock::now();
						file << std::format("[{:%H:%M:%S}] [{}] {}\n",
							std::chrono::floor<std::chrono::milliseconds>(now),
							a_level == 2 ? "ERROR" : a_level == 1 ? "WARN" : "info", a_text);
						file.flush();
					}
				}
				// Warnings/errors also reach the game's own log; info only via InfoFwd.
				if (a_level > 0) {
					Forward(a_level, a_text);
				}
			}

			void Forward(int a_level, const std::string& a_text)
			{
				if (pipe) {
					pipe->WriteMessage(json{
						{ "type", "log" }, { "level", a_level }, { "text", a_text } }.dump());
				}
			}

			void Info(const std::string& a_text) { Log(0, a_text); }
			// Info that also reaches the game log (milestones).
			void InfoFwd(const std::string& a_text)
			{
				Log(0, a_text);
				Forward(0, a_text);
			}
			void Warn(const std::string& a_text) { Log(1, a_text); }
			void Error(const std::string& a_text) { Log(2, a_text); }
		};

		// Microsoft's permanent link to the WebView2 Evergreen Bootstrapper
		// (see /microsoft-edge/webview2/concepts/distribution, "online-only
		// deployment") — opening it downloads MicrosoftEdgeWebview2Setup.exe.
		constexpr wchar_t kRuntimeDownloadUrl[] =
			L"https://go.microsoft.com/fwlink/p/?LinkId=2124703";

		// A missing Evergreen runtime leaves the overlay invisible, and players
		// don't read logs — raise a real dialog. This process lives outside the
		// game, so parking a throwaway thread in MessageBox is safe and topmost
		// works over the (borderless) game window. At most one prompt per host.
		void PromptInstallWebView2Runtime(Logger& a_log)
		{
			static std::atomic_bool prompted{ false };
			if (prompted.exchange(true)) return;
			a_log.Error(
				"the WebView2 Evergreen runtime is not installed — showing the "
				"install dialog (download: "
				"https://go.microsoft.com/fwlink/p/?LinkId=2124703)");
			std::thread([] {
				const auto choice = ::MessageBoxW(nullptr,
					L"OSF UI cannot start because the Microsoft Edge WebView2 "
					L"Runtime is not installed on this PC.\n\n"
					L"The in-game overlay (Mods menu / mod settings) will not "
					L"appear without it.\n\n"
					L"Open the download in your browser now? Run the downloaded "
					L"\"MicrosoftEdgeWebview2Setup.exe\", then restart the game.",
					// ASCII only: MSVC parses this file as ANSI, so non-ASCII
					// in wide literals turns to mojibake (narrow literals pass
					// through as raw UTF-8 and are fine).
					L"OSF UI - WebView2 Runtime missing",
					MB_YESNO | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
				if (choice == IDYES) {
					::ShellExecuteW(nullptr, L"open", kRuntimeDownloadUrl,
						nullptr, nullptr, SW_SHOWNORMAL);
				}
			}).detach();
		}

		struct App
		{
			HostOptions options;
			Logger      log;
			Pipe        pipe;
			HANDLE      gameProcess{ nullptr };
			HANDLE      wakeEvent{ nullptr };
			std::thread reader;
			std::atomic_bool quit{ false };
			std::string      byeReason;  // overrides the default bye reason (STA thread only)

			std::mutex       commandMutex;
			std::deque<json> commands;
			std::atomic_bool pipeDead{ false };

			// Init state from the game.
			bool                  initialized{ false };
			HWND                  gameTopLevel{ nullptr };
			std::filesystem::path viewsRoot, userData;
			std::wstring          virtualHost{ L"osfui.local" };
			std::uint32_t         width{ 1 }, height{ 1 };
			bool                  devMode{ false };
			bool                  defaultHidden{ true };  // init.hidden — a new view's starting state

			HWND bootstrapWindow{ nullptr };
			HWND hostWindow{ nullptr };
			WNDPROC hostWindowProc{ nullptr };
			// A WNDPROC cannot carry state; one App exists per host process.
			static inline App* s_hostInputApp{ nullptr };
			bool reparented{ false };

			// One view = one composition controller + WebView2 targeting its own
			// child ContainerVisual of the captured root, plus a 1x1 child HWND of
			// hostWindow so focus and synthetic keys route per view. Only the root
			// is captured: WGC sees the already-composited stack, so N views still
			// cost one capture and one texture ring.
			struct View
			{
				std::string id;
				HWND        window{ nullptr };
				winrt::Windows::UI::Composition::ContainerVisual visual{ nullptr };
				ComPtr<ICoreWebView2Controller>            controller;
				ComPtr<ICoreWebView2CompositionController> compositionController;
				ComPtr<ICoreWebView2>                      webView;
				ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> consoleReceiver;
				bool controllerRequested{ false };
				bool hidden{ true };
				// One-shot hidden paint requested by the game. Unlike leaving the
				// controller visible indefinitely, this primes Chromium without
				// running closed-view animations for the rest of the session.
				bool prewarm{ false };
				bool prewarmPending{ false };
				std::uint64_t prewarmDeadline{ 0 };
				bool renderStats{ false };
				std::uint64_t renderStatsLastPageLogMs{ 0 };
				// Manifest (authoring) height, set by `navigate`: the page lays out at
				// this height and ApplyScale derives the rasterization scale from it.
				std::uint32_t logicalHeight{ kDefaultLogicalHeight };
				// Deferred visibility: a reveal waits for the page's first painted
				// frame after Chromium resume, and hides wait for pending reveals.
				bool          revealPending{ false };
				bool          hideDeferred{ false };
				std::uint64_t revealDeadline{ 0 };
				int  order{ 0 };
				bool domSeen{ false }, navigationSucceeded{ false }, domNotified{ false };
				std::wstring currentUrl;
				std::optional<std::wstring> pendingNavigate;
				std::deque<std::string> queuedPostWeb;
				struct QueuedEval { std::uint64_t id; std::string script; };
				std::deque<QueuedEval> queuedEvals;
			};
			std::vector<std::unique_ptr<View>> views;  // creation order (= z tie-break)
			View* active{ nullptr };  // mouse/focus/synthetic-key target
			bool  captureStarted{ false };

			// accel state pushed by the game (touched only on the STA thread)
			std::uint32_t toggleVk{ 0x79 /*F10*/ }, devReloadVk{ 0 }, captureUpVk{ 0 };
			bool          captured{ false }, captureArmed{ false };
			// Whether an input-capturing menu owns real OS focus. HUD-only views
			// leave this false so Starfield stays foreground. During a grant the
			// host HWND captures legacy mouse input; keyboard/IME route naturally
			// to Chromium, and the game polls XInput independently of its suspended
			// Windows.Gaming.Input stream.
			bool          focusGranted{ false };
			int           capturedMouseX{ 0 }, capturedMouseY{ 0 };
			std::unordered_set<UINT> handledKeys;
			// Warn-once dedupe for denied egress ("viewId|host"): a page that
			// retries a blocked fetch in a loop must not spam the log.
			std::unordered_set<std::string> egressWarned;
			std::uint64_t accelEvents{ 0 };  // every AcceleratorKeyPressed callback (diagnostic)
			// NOTE: the "key" command used to PostMessage into the Chromium widget
			// and mark each tap in a syntheticKeys map so AcceleratorKeyPressed
			// would pass it to the page instead of round-tripping it to the game
			// (a delegated Esc read as a fresh press caused an infinite ping-pong).
			// Synthetic gamepad keys are delivered as DOM events by the bridge
			// shim — no Win32 message or marker needed. Physical presses reach the
			// accelerator path throughout an interactive-menu session.

			// Rebind capture of character keys. AcceleratorKeyPressed, this host's
			// only key path, by design does not fire for keys that map to a character
			// with neither Ctrl nor Alt held ("A key is considered an accelerator
			// if ... the pressed key does not map to a character"), so F-keys, Esc
			// and arrows rebind but letters and digits never reach the game. While a
			// capture is armed, the Chromium focus widget's session subclass forwards
			// WM_KEYDOWN over the same "accelerator" message. That subclass also
			// catches focused WM_MOUSEWHEEL during interactive menus. It lives in the
			// host rather than the game, so it cannot collide with SFSE WndProc hooks.
			HWND    captureWidget{ nullptr };
			WNDPROC captureWidgetProc{ nullptr };
			// A WNDPROC cannot carry state and there is one App per host process.
			// Set only while the subclass is installed.
			static inline App* s_app{ nullptr };

			ComPtr<ID3D11Device>         device;
			ComPtr<ID3D11Device5>        device5;
			ComPtr<ID3D11DeviceContext>  context;
			ComPtr<ID3D11DeviceContext4> context4;
			LUID                         graphicsAdapterLuid{};

			winrt::Windows::System::DispatcherQueueController dispatcher{ nullptr };
			winrt::Windows::UI::Composition::Compositor compositor{ nullptr };
			winrt::Windows::UI::Composition::ContainerVisual rootVisual{ nullptr };
			winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice captureDevice{ nullptr };
			winrt::Windows::Graphics::Capture::GraphicsCaptureItem captureItem{ nullptr };
			winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool{ nullptr };
			winrt::Windows::Graphics::Capture::GraphicsCaptureSession captureSession{ nullptr };
			winrt::event_token frameToken{};
			std::atomic_bool   captureClosing{ true };
			std::uint32_t      captureCadenceHz{ 0 };

			// Shared texture ring: the capture thread owns it; ringMutex guards
			// against teardown from the STA thread. WGC dirty rectangles are relative
			// to the immediately previous capture, but a reused ring slot contains a
			// frame from several publishes ago. Copying only the current dirty rects
			// would therefore leave stale pixels; keep full copies unless per-slot
			// dirty-history accumulation is added.
			std::mutex ringMutex;
			struct Slot
			{
				ComPtr<ID3D11Texture2D> texture;
				HANDLE                  localHandle{ nullptr };
				std::uint64_t           lastSerial{ 0 };
			};
			std::array<Slot, kRingSlots> ring{};
			std::uint32_t ringWidth{ 0 }, ringHeight{ 0 };
			std::uint32_t ringWrite{ 0 };
			bool          ringKeyedMutex{ false };
			ComPtr<ID3D11Fence> produceFence, consumeFence;
			std::uint64_t frameSerial{ 0 };
			std::uint32_t lastSlot{ 0 };
			bool          anyFramePublished{ false };
			// Serials the game released without a GPU read (hidden overlay, stale
			// ring): it has no device to CPU-signal the consume fence, so it acks
			// over the pipe instead.
			std::atomic<std::uint64_t> ackedSerial{ 0 };
			std::uint64_t consumeWaitTimeouts{ 0 };
			double        produceMsTotal{ 0.0 };
			std::uint64_t produceCount{ 0 };

			// Capture-cadence diagnostics (the benchmark's 48 fps ceiling): the
			// interval between WGC FrameArrived callbacks is DWM's commit cadence
			// for the captured visual, i.e. the transport's input rate, upstream of
			// anything the host controls. Touched only on the capture callback thread.
			std::chrono::steady_clock::time_point captureLastArrival{};
			double        captureGapMsTotal{ 0.0 };
			double        captureGapMsMin{ 0.0 };
			double        captureGapMsMax{ 0.0 };
			std::uint64_t captureGapCount{ 0 };
			std::atomic<std::uint64_t> captureArrivalCount{ 0 };
			std::uint64_t statsLastMs{ 0 };
			std::uint64_t statsLastCapture{ 0 };
			std::uint64_t statsLastPublish{ 0 };
			double        statsLastProduceMs{ 0.0 };
			std::uint64_t statsLastTimeouts{ 0 };
			std::uint64_t statsLastLogMs{ 0 };
			json          compositorStats{ json::object() };

			ComPtr<ICoreWebView2Environment> environment;
			bool environmentRequested{ false };

			void Send(const json& a_msg) { pipe.WriteMessage(a_msg.dump()); }

			void ReaderMain()
			{
				std::string payload;
				while (pipe.ReadMessage(payload)) {
					json parsed = json::parse(payload, nullptr, false);
					if (parsed.is_discarded()) {
						log.Warn("dropping unparseable pipe message");
						continue;
					}
					{
						std::scoped_lock lock(commandMutex);
						commands.push_back(std::move(parsed));
					}
					::SetEvent(wakeEvent);
				}
				pipeDead.store(true);
				::SetEvent(wakeEvent);
			}

			bool InitializeGraphics(const std::optional<LUID>& a_requestedLuid)
			{
				ComPtr<IDXGIAdapter1> selectedAdapter;
				if (a_requestedLuid) {
					ComPtr<IDXGIFactory1> factory;
					const auto factoryHr = ::CreateDXGIFactory1(IID_PPV_ARGS(&factory));
					if (SUCCEEDED(factoryHr)) {
						for (UINT index = 0; ; ++index) {
							ComPtr<IDXGIAdapter1> candidate;
							if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
							DXGI_ADAPTER_DESC1 desc{};
							if (SUCCEEDED(candidate->GetDesc1(&desc)) &&
								desc.AdapterLuid.LowPart == a_requestedLuid->LowPart &&
								desc.AdapterLuid.HighPart == a_requestedLuid->HighPart) {
								selectedAdapter = std::move(candidate);
								break;
							}
						}
					} else {
						log.Warn(std::format(
							"CreateDXGIFactory1 failed while matching the game adapter (0x{:08X}); "
							"falling back to the host default GPU", static_cast<unsigned>(factoryHr)));
					}
					if (!selectedAdapter) {
						log.Warn(std::format(
							"game adapter LUID 0x{:08X}:0x{:08X} was not found in the host; "
							"falling back to the host default GPU",
							static_cast<std::uint32_t>(a_requestedLuid->HighPart),
							a_requestedLuid->LowPart));
					}
				}

				const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
				D3D_FEATURE_LEVEL actual{};
				auto hr = ::D3D11CreateDevice(selectedAdapter.Get(),
					selectedAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
					nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
					static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
					&device, &actual, &context);
				if (FAILED(hr)) {
					log.Error(std::format("D3D11CreateDevice failed (0x{:08X})", static_cast<unsigned>(hr)));
					return false;
				}
				ComPtr<ID3D10Multithread> multithread;
				if (SUCCEEDED(context.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
				if (FAILED(device.As(&device5)) || FAILED(context.As(&context4))) {
					log.Error("ID3D11Device5/DeviceContext4 unavailable (need Win10 1703+) — no shared-fence transport");
					return false;
				}
				ComPtr<IDXGIDevice> dxgi;
				if (FAILED(device.As(&dxgi))) return false;
				ComPtr<IDXGIAdapter> actualAdapter;
				DXGI_ADAPTER_DESC actualDesc{};
				if (FAILED(dxgi->GetAdapter(&actualAdapter)) ||
					FAILED(actualAdapter->GetDesc(&actualDesc))) {
					log.Error("could not identify the D3D11 capture adapter");
					return false;
				}
				graphicsAdapterLuid = actualDesc.AdapterLuid;
				log.InfoFwd(std::format("D3D11 capture adapter '{}' LUID 0x{:08X}:0x{:08X}",
					ToUtf8(actualDesc.Description),
					static_cast<std::uint32_t>(graphicsAdapterLuid.HighPart),
					graphicsAdapterLuid.LowPart));

				winrt::com_ptr<::IInspectable> inspectable;
				hr = ::CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put());
				if (FAILED(hr)) {
					log.Error(std::format("CreateDirect3D11DeviceFromDXGIDevice failed (0x{:08X})",
						static_cast<unsigned>(hr)));
					return false;
				}
				captureDevice = inspectable.as<
					winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
				return true;
			}

			bool InitializeComposition()
			{
				DispatcherQueueOptions dq{ sizeof(DispatcherQueueOptions),
					DQTYPE_THREAD_CURRENT, DQTAT_COM_STA };
				const auto hr = ::CreateDispatcherQueueController(dq,
					reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(
						winrt::put_abi(dispatcher)));
				if (FAILED(hr)) {
					log.Error(std::format("CreateDispatcherQueueController failed (0x{:08X})",
						static_cast<unsigned>(hr)));
					return false;
				}
				compositor = winrt::Windows::UI::Composition::Compositor();
				rootVisual = compositor.CreateContainerVisual();
				rootVisual.Size({ static_cast<float>(width), static_cast<float>(height) });
				// The root stays visible for the lifetime of the capture; per-view
				// visibility lives on each view's child visual.
				rootVisual.IsVisible(true);
				return true;
			}

			bool CreateWindows()
			{
				// A visible 1x1 child beneath a visible (offscreen) top-level owned
				// by this STA; the child is reparented beneath the game window once
				// Chromium is up.
				bootstrapWindow = ::CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC",
					L"OSFUI WebView2 Host Bootstrap", WS_POPUP | WS_VISIBLE,
					-32000, -32000, 1, 1, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
				if (!bootstrapWindow) {
					log.Error(std::format("bootstrap HWND creation failed ({})", ::GetLastError()));
					return false;
				}
				hostWindow = ::CreateWindowExW(0, L"STATIC", L"OSFUI WebView2 Host",
					WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, bootstrapWindow, nullptr,
					::GetModuleHandleW(nullptr), nullptr);
				if (!hostWindow) {
					log.Error(std::format("host child HWND creation failed ({})", ::GetLastError()));
					return false;
				}
				s_hostInputApp = this;
				::SetLastError(ERROR_SUCCESS);
				hostWindowProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
					hostWindow, GWLP_WNDPROC,
					reinterpret_cast<LONG_PTR>(&HostInputWndProc)));
				if (!hostWindowProc && ::GetLastError() != ERROR_SUCCESS) {
					log.Error(std::format("host child HWND subclass failed ({})", ::GetLastError()));
					s_hostInputApp = nullptr;
					return false;
				}
				return true;
			}

			void ReleaseRing()
			{
				for (auto& slot : ring) {
					if (slot.localHandle) {
						::CloseHandle(slot.localHandle);
						slot.localHandle = nullptr;
					}
					slot.texture.Reset();
					slot.lastSerial = 0;
				}
				ringWidth = ringHeight = 0;
				ringWrite = 0;
			}

			bool EnsureFences()
			{
				if (produceFence && consumeFence) return true;
				auto hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
					IID_PPV_ARGS(&produceFence));
				if (FAILED(hr)) {
					log.Error(std::format("CreateFence(produce) failed (0x{:08X})",
						static_cast<unsigned>(hr)));
					return false;
				}
				hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
					IID_PPV_ARGS(&consumeFence));
				if (FAILED(hr)) {
					log.Error(std::format("CreateFence(consume) failed (0x{:08X})",
						static_cast<unsigned>(hr)));
					return false;
				}
				return true;
			}

			[[nodiscard]] HANDLE DuplicateToGame(HANDLE a_local)
			{
				HANDLE remote = nullptr;
				if (!::DuplicateHandle(::GetCurrentProcess(), a_local, gameProcess,
						&remote, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
					log.Error(std::format("DuplicateHandle into game failed ({})", ::GetLastError()));
					return nullptr;
				}
				return remote;
			}

			// Capture thread. Returns false when the ring could not be built.
			bool EnsureRing(std::uint32_t a_width, std::uint32_t a_height)
			{
				if (ring[0].texture && ringWidth == a_width && ringHeight == a_height) {
					return true;
				}
				ReleaseRing();
				if (!EnsureFences()) return false;

				D3D11_TEXTURE2D_DESC desc{};
				desc.Width = a_width;
				desc.Height = a_height;
				desc.MipLevels = 1;
				desc.ArraySize = 1;
				desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
				desc.SampleDesc.Count = 1;
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
				// Preferred: NT-handle shared without a keyed mutex — the D3D12 side
				// has no IDXGIKeyedMutex; the shared fences do the synchronizing.
				desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
				ringKeyedMutex = false;
				auto hr = device->CreateTexture2D(&desc, nullptr, &ring[0].texture);
				if (FAILED(hr)) {
					// Some drivers only accept NTHANDLE together with KEYED_MUTEX.
					desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
						D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
					ringKeyedMutex = true;
					hr = device->CreateTexture2D(&desc, nullptr, &ring[0].texture);
					if (FAILED(hr)) {
						log.Error(std::format(
							"shared texture creation failed both modes (0x{:08X})",
							static_cast<unsigned>(hr)));
						return false;
					}
				}
				for (std::uint32_t i = 1; i < kRingSlots; ++i) {
					if (FAILED(device->CreateTexture2D(&desc, nullptr, &ring[i].texture))) {
						log.Error("shared texture ring creation failed");
						ReleaseRing();
						return false;
					}
				}
				for (auto& slot : ring) {
					ComPtr<IDXGIResource1> resource;
					if (FAILED(slot.texture.As(&resource)) ||
						FAILED(resource->CreateSharedHandle(nullptr,
							DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
							nullptr, &slot.localHandle))) {
						log.Error("CreateSharedHandle failed");
						ReleaseRing();
						return false;
					}
				}
				ringWidth = a_width;
				ringHeight = a_height;
				ringWrite = 0;

				// Duplicate everything into the game and announce the new ring.
				json slots = json::array();
				for (auto& slot : ring) {
					const auto remote = DuplicateToGame(slot.localHandle);
					if (!remote) {
						ReleaseRing();
						return false;
					}
					slots.push_back(reinterpret_cast<std::uint64_t>(remote));
				}
				HANDLE produceLocal = nullptr;
				HANDLE consumeLocal = nullptr;
				if (FAILED(produceFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
						&produceLocal)) ||
					FAILED(consumeFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
						&consumeLocal))) {
					log.Error("fence CreateSharedHandle failed");
					ReleaseRing();
					return false;
				}
				const auto produceRemote = DuplicateToGame(produceLocal);
				const auto consumeRemote = DuplicateToGame(consumeLocal);
				::CloseHandle(produceLocal);
				::CloseHandle(consumeLocal);
				if (!produceRemote || !consumeRemote) {
					ReleaseRing();
					return false;
				}
				Send(json{
					{ "type", "textures" },
					{ "width", a_width },
					{ "height", a_height },
					{ "slots", std::move(slots) },
					{ "produceFence", reinterpret_cast<std::uint64_t>(produceRemote) },
					{ "consumeFence", reinterpret_cast<std::uint64_t>(consumeRemote) },
					{ "keyedMutex", ringKeyedMutex },
					{ "adapterLuidLow", graphicsAdapterLuid.LowPart },
					{ "adapterLuidHigh", static_cast<std::uint32_t>(graphicsAdapterLuid.HighPart) },
				});
				log.InfoFwd(std::format(
					"shared texture ring ready {}x{} ({} slots, keyedMutex={})",
					a_width, a_height, kRingSlots, ringKeyedMutex));
				return true;
			}

			// Capture thread: publish one captured surface through the ring.
			void PublishFrame(ID3D11Texture2D* a_source, std::uint32_t a_width, std::uint32_t a_height)
			{
				const auto start = std::chrono::steady_clock::now();
				const auto sourceTimeMs = ::GetTickCount64();
				std::scoped_lock lock(ringMutex);
				if (captureClosing.load()) return;
				if (!EnsureRing(a_width, a_height)) return;

				auto& slot = ring[ringWrite];
				// Slot reuse guard: the game GPU-signals `consume` with each serial it
				// finished reading and pipe-acks frames it skipped, so this wait is
				// normally already satisfied. Bounded, so a wedged consumer costs
				// 50 ms rather than a deadlock.
				const auto consumed = [this] {
					return (std::max)(consumeFence->GetCompletedValue(), ackedSerial.load());
				};
				if (slot.lastSerial != 0 && consumed() < slot.lastSerial) {
					const HANDLE evt = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
					if (evt) {
						consumeFence->SetEventOnCompletion(slot.lastSerial, evt);
						const auto deadline = ::GetTickCount64() + 50;
						while (consumed() < slot.lastSerial && ::GetTickCount64() < deadline) {
							::WaitForSingleObject(evt, 10);
						}
						if (consumed() < slot.lastSerial) {
							++consumeWaitTimeouts;
							if (consumeWaitTimeouts == 1 || consumeWaitTimeouts % 300 == 0) {
								log.Warn(std::format(
									"consume lagging (slot serial {}, completed {}, {} timeouts) — overwriting",
									slot.lastSerial, consumed(), consumeWaitTimeouts));
							}
						}
						::CloseHandle(evt);
					}
				}

				if (ringKeyedMutex) {
					ComPtr<IDXGIKeyedMutex> mutex;
					if (SUCCEEDED(slot.texture.As(&mutex))) {
						if (mutex->AcquireSync(0, 50) != S_OK) {
							return;  // contended/abandoned; drop this frame
						}
						context->CopyResource(slot.texture.Get(), a_source);
						mutex->ReleaseSync(0);
					} else {
						return;  // keyed-mutex QI unexpectedly failed; drop rather than publish an uncopied slot
					}
				} else {
					context->CopyResource(slot.texture.Get(), a_source);
				}

				const auto serial = ++frameSerial;
				slot.lastSerial = serial;
				lastSlot = ringWrite;
				ringWrite = (ringWrite + 1) % kRingSlots;
				context4->Signal(produceFence.Get(), serial);
				// Flush so the copy + signal reach the GPU now: the consumer's wait
				// must not depend on this context's next natural flush.
				context->Flush();
				anyFramePublished = true;
				Send(json{
					{ "type", "frame" }, { "slot", lastSlot }, { "serial", serial },
					{ "width", a_width }, { "height", a_height },
					{ "sourceTimeMs", sourceTimeMs } });

				produceMsTotal += std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - start).count();
				++produceCount;
				if (serial == 1) {
					log.InfoFwd(std::format("first frame published ({}x{})", a_width, a_height));
				}
			}

			// STA thread, on unhide: the runtime's reveal gate needs a fresh serial,
			// but a static page paints nothing new — resend the newest pixels under
			// a new serial.
			void RepublishLatest()
			{
				std::scoped_lock lock(ringMutex);
				// The last slot must actually hold pixels: after a resize recreated
				// the ring, nothing is republishable until the first capture lands
				// in the new ring.
				if (!anyFramePublished || !ring[0].texture ||
					ring[lastSlot].lastSerial == 0) {
					return;
				}
				const auto serial = ++frameSerial;
				ring[lastSlot].lastSerial = serial;
				context4->Signal(produceFence.Get(), serial);
				context->Flush();
				Send(json{
					{ "type", "frame" }, { "slot", lastSlot }, { "serial", serial },
					{ "width", ringWidth }, { "height", ringHeight },
					{ "sourceTimeMs", ::GetTickCount64() } });
			}

			View* FindView(std::string_view a_id)
			{
				for (auto& view : views) {
					if (view->id == a_id) return view.get();
				}
				return nullptr;
			}

			// View-scoped messages carry `view`; absent or unknown falls back to the
			// active view.
			View* ResolveView(const json& a_msg)
			{
				if (const auto it = a_msg.find("view"); it != a_msg.end() && it->is_string()) {
					if (auto* view = FindView(it->get<std::string>())) return view;
				}
				return active;
			}

			View& CreateView(const std::string& a_id)
			{
				auto owned = std::make_unique<View>();
				owned->id = a_id;
				owned->hidden = defaultHidden;
				owned->window = ::CreateWindowExW(0, L"STATIC", L"OSFUI WebView2 View",
					WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, hostWindow, nullptr,
					::GetModuleHandleW(nullptr), nullptr);
				if (!owned->window) {
					log.Error(std::format("view '{}': child HWND creation failed ({})",
						a_id, ::GetLastError()));
				}
				views.push_back(std::move(owned));
				auto& view = *views.back();
				if (!active) active = &view;
				RequestController(view);
				return view;
			}

			// View order maps to child order under the captured root: lower `order`
			// composites beneath, ties keep creation order. Rebuilt wholesale;
			// reorders are rare and the child count tiny.
			void ReorderVisuals()
			{
				if (!rootVisual) return;
				auto children = rootVisual.Children();
				children.RemoveAll();
				std::vector<View*> sorted;
				for (auto& view : views) {
					if (view->visual) sorted.push_back(view.get());
				}
				std::stable_sort(sorted.begin(), sorted.end(),
					[](const View* a_a, const View* a_b) { return a_a->order < a_b->order; });
				for (auto* view : sorted) {
					children.InsertAtTop(view->visual);
				}
			}

			// Deferred visibility. A hidden view's controller gets
			// put_IsVisible(FALSE), which suspends Chromium rendering, so on unhide
			// it needs a few frames before it paints. A menu switch arrives as
			// hide-old + show-new in one policy batch, so applying it verbatim blanks
			// the output for those frames. Instead: resume Chromium at once but keep
			// the child visual hidden until the page confirms a painted frame
			// (double-rAF sentinel posted as a web message the host intercepts), and
			// hold the batch's hides until every pending reveal completes or times
			// out — the old content stays up and the switch is one composition change.

			static constexpr const wchar_t* kRevealSentinelScript =
				L"requestAnimationFrame(function(){requestAnimationFrame(function(){"
				L"chrome.webview.postMessage('__osfuiRevealReady');});});";
			static constexpr std::string_view kRevealSentinel = "__osfuiRevealReady";
			static constexpr const wchar_t* kPrewarmSentinelScript =
				L"requestAnimationFrame(function(){requestAnimationFrame(function(){"
				L"setTimeout(function(){chrome.webview.postMessage('__osfuiPrewarmReady');},0);"
				L"});});";
			static constexpr std::string_view kPrewarmSentinel = "__osfuiPrewarmReady";
			static constexpr std::uint64_t kRevealTimeoutMs = 300;

			void BeginPrewarm(View& a_view)
			{
				if (!a_view.prewarm || !a_view.hidden) return;
				if (!a_view.prewarmPending) {
					a_view.prewarmPending = true;
					a_view.prewarmDeadline = 0;
					if (a_view.controller) a_view.controller->put_IsVisible(TRUE);
				}
				// The request may arrive before navigation reaches DOMContentLoaded.
				// Calling again there arms the paint handshake once rAF exists.
				if (a_view.webView && a_view.domSeen) {
					a_view.prewarmDeadline = ::GetTickCount64() + kRevealTimeoutMs;
					a_view.webView->ExecuteScript(kPrewarmSentinelScript,
						Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
							[](HRESULT, LPCWSTR) -> HRESULT { return S_OK; }).Get());
				}
			}

			void CompletePrewarm(View& a_view)
			{
				if (!a_view.prewarmPending) return;
				a_view.prewarmPending = false;
				a_view.prewarmDeadline = 0;
				if (a_view.hidden && a_view.controller) {
					a_view.controller->put_IsVisible(FALSE);
				}
				log.Info(std::format("view '{}': hidden prewarm complete", a_view.id));
			}

			bool AnyRevealPending() const
			{
				for (const auto& view : views) {
					if (view->revealPending) return true;
				}
				return false;
			}

			void ApplyDeferredHides()
			{
				for (auto& view : views) {
					if (!view->hideDeferred) continue;
					view->hideDeferred = false;
					if (view->visual) view->visual.IsVisible(false);
					if (view->controller) view->controller->put_IsVisible(FALSE);
				}
			}

			void HideView(View& a_view)
			{
				if (a_view.hidden && !a_view.revealPending) return;
				a_view.hidden = true;
				a_view.revealPending = false;  // cancel an in-flight reveal
				a_view.prewarmPending = false;
				a_view.prewarmDeadline = 0;
				a_view.hideDeferred = true;    // applied at batch end / reveal end
				log.Info(std::format("view '{}': hide (deferred to batch end)", a_view.id));
			}

			void ShowView(View& a_view)
			{
				if (!a_view.hidden) {
					a_view.hideDeferred = false;
					log.Info(std::format("view '{}': show — already visible (visual={})",
						a_view.id, a_view.visual && a_view.visual.IsVisible()));
					return;
				}
				a_view.hidden = false;
				a_view.hideDeferred = false;
				a_view.prewarmPending = false;
				a_view.prewarmDeadline = 0;
				// Resume Chromium first — nothing paints while suspended.
				if (a_view.controller) a_view.controller->put_IsVisible(TRUE);
				if (a_view.visual && a_view.visual.IsVisible()) {
					log.Info(std::format(
						"view '{}': show — hide was still deferred, never left the screen", a_view.id));
					return;
				}
				if (a_view.visual && a_view.webView && a_view.domSeen) {
					a_view.revealPending = true;
					a_view.revealDeadline = ::GetTickCount64() + kRevealTimeoutMs;
					log.Info(std::format("view '{}': show — reveal pending ({} ms timeout)",
						a_view.id, kRevealTimeoutMs));
					a_view.webView->ExecuteScript(kRevealSentinelScript,
						Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
							[](HRESULT, LPCWSTR) -> HRESULT { return S_OK; }).Get());
				} else {
					// No page to ask (still loading / no controller yet): show
					// directly; the runtime's overlay reveal gate covers boot.
					log.Info(std::format(
						"view '{}': show — direct (visual={} webView={} domSeen={})", a_view.id,
						a_view.visual != nullptr, a_view.webView != nullptr, a_view.domSeen));
					if (a_view.visual) a_view.visual.IsVisible(true);
					RepublishLatest();
				}
			}

			void CompleteReveal(View& a_view, bool a_timedOut)
			{
				if (!a_view.revealPending) return;
				a_view.revealPending = false;
				if (a_timedOut) {
					log.Info(std::format(
						"view '{}': reveal sentinel timed out — showing anyway", a_view.id));
				} else {
					log.Info(std::format("view '{}': reveal sentinel arrived — showing", a_view.id));
				}
				if (a_view.visual && !a_view.hidden) a_view.visual.IsVisible(true);
				if (!AnyRevealPending()) ApplyDeferredHides();
				// Fresh serial for the runtime's reveal gate: an unchanged page may
				// otherwise never produce a new captured frame.
				RepublishLatest();
			}

			void TickReveals()
			{
				const auto now = ::GetTickCount64();
				for (auto& view : views) {
					if (view->revealPending && now >= view->revealDeadline) {
						CompleteReveal(*view, /*a_timedOut=*/true);
					}
					if (view->prewarmPending && view->prewarmDeadline != 0 &&
						now >= view->prewarmDeadline) {
						log.Info(std::format(
							"view '{}': hidden prewarm sentinel timed out", view->id));
						CompletePrewarm(*view);
					}
				}
			}

			bool AnyVisibleRenderStats() const
			{
				for (const auto& view : views) {
					if (view->renderStats && !view->hidden && view->webView) return true;
				}
				return false;
			}

			void ApplyRenderStats(View& a_view)
			{
				if (!a_view.webView) return;
				const auto script = std::format(
					"window.__osfuiSetRenderStats&&window.__osfuiSetRenderStats({},{});",
					a_view.renderStats ? "true" : "false", json(a_view.id).dump());
				a_view.webView->ExecuteScript(ToWide(script).c_str(),
					Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
						[](HRESULT, LPCWSTR) -> HRESULT { return S_OK; }).Get());
			}

			void TickRenderStats()
			{
				if (!AnyVisibleRenderStats()) {
					statsLastMs = 0;
					statsLastLogMs = 0;
					return;
				}
				const auto now = ::GetTickCount64();
				if (statsLastMs == 0) {
					statsLastMs = now;
					statsLastCapture = captureArrivalCount.load(std::memory_order_relaxed);
					std::scoped_lock lock(ringMutex);
					statsLastPublish = frameSerial;
					statsLastProduceMs = produceMsTotal;
					statsLastTimeouts = consumeWaitTimeouts;
					return;
				}
				const auto elapsed = now - statsLastMs;
				if (elapsed < 500) return;

				const auto capture = captureArrivalCount.load(std::memory_order_relaxed);
				std::uint64_t publish = 0, timeouts = 0;
				double produceMs = 0.0;
				{
					std::scoped_lock lock(ringMutex);
					publish = frameSerial;
					produceMs = produceMsTotal;
					timeouts = consumeWaitTimeouts;
				}
				const auto captureDelta = capture - statsLastCapture;
				const auto publishDelta = publish - statsLastPublish;
				const auto timeoutDelta = timeouts - statsLastTimeouts;
				const double seconds = static_cast<double>(elapsed) / 1000.0;
				const double copyMs = publishDelta ?
					(produceMs - statsLastProduceMs) / static_cast<double>(publishDelta) : 0.0;
				json values{
					{ "captureFps", static_cast<double>(captureDelta) / seconds },
					{ "transferFps", static_cast<double>(publishDelta) / seconds },
					{ "copyMs", copyMs },
					{ "backpressure", timeoutDelta }
				};
				values.update(compositorStats);
				const json sample{ { "__osfuiRenderStats", values } };
				const auto wide = ToWide(sample.dump());
				for (const auto& view : views) {
					if (view->renderStats && !view->hidden && view->webView) {
						view->webView->PostWebMessageAsJson(wide.c_str());
					}
				}
				if (statsLastLogMs == 0 || now - statsLastLogMs >= 2000) {
					statsLastLogMs = now;
					log.InfoFwd(std::format(
						"render diagnostics: WGC capture {:.1f} fps, shared-ring publish {:.1f} fps, "
						"publish CPU {:.3f} ms, backpressure timeouts {}",
						static_cast<double>(captureDelta) / seconds,
						static_cast<double>(publishDelta) / seconds,
						copyMs, timeoutDelta));
				}
				statsLastMs = now;
				statsLastCapture = capture;
				statsLastPublish = publish;
				statsLastProduceMs = produceMs;
				statsLastTimeouts = timeouts;
			}

			void DestroyOneView(View& a_view)
			{
				// The session-input subclass may be sitting on this view's widget: unhook
				// before the HWND goes away.
				if (captureWidget && ::IsChild(a_view.window, captureWidget)) {
					RemoveCaptureSubclass();
				}
				if (a_view.compositionController) {
					a_view.compositionController->put_RootVisualTarget(nullptr);
				}
				if (a_view.controller) {
					a_view.controller->Close();
				}
				if (a_view.visual) {
					if (rootVisual) rootVisual.Children().Remove(a_view.visual);
					a_view.visual = nullptr;
				}
				a_view.consoleReceiver.Reset();
				a_view.webView.Reset();
				a_view.compositionController.Reset();
				a_view.controller.Reset();
				if (a_view.window) {
					::DestroyWindow(a_view.window);
					a_view.window = nullptr;
				}
			}

			bool BeginEnvironment()
			{
				if (environmentRequested) return true;
				environmentRequested = true;
				std::error_code ec;
				std::filesystem::create_directories(userData, ec);
				const auto callback =
					Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
						[this](HRESULT a_hr, ICoreWebView2Environment* a_environment) -> HRESULT {
							if (quit.load()) return S_OK;
							if (FAILED(a_hr) || !a_environment) {
								log.Error(std::format("environment callback failed (0x{:08X})",
									static_cast<unsigned>(a_hr)));
								if (a_hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
									PromptInstallWebView2Runtime(log);
								}
								return S_OK;
							}
							environment = a_environment;
							// Views navigated before the environment came up have
							// been waiting for their controllers.
							for (auto& view : views) {
								RequestController(*view);
							}
							return S_OK;
						});
				// This host is intentionally windowless in practice: Chromium renders
				// into a DirectComposition visual that WGC captures, while the native
				// owner stays a visible 1x1 child so it cannot cover or intercept the
				// game. Chromium's Windows occlusion tracker can classify that tiny
				// owner as occluded and background the otherwise-visible controller;
				// telemetry then shows rAF snapping from the monitor cadence to ~24-30
				// fps. Ignore native HWND occlusion for this capture-only browser. Keep
				// visible renderers at foreground scheduling priority as well. The
				// native owner never receives ordinary foreground activation, so Chromium
				// can otherwise demote a busy renderer even after native occlusion
				// backgrounding is disabled. Native occlusion can also be applied directly
				// to Chromium's compositor through a separately field-trialled feature, so
				// disable both the calculation and compositor policy for this capture-only
				// HWND. Explicit put_IsVisible(FALSE) remains the lifecycle gate that
				// suspends hidden OSF UI views.
				auto environmentOptions = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
				if (!environmentOptions) {
					log.Error("could not allocate WebView2 environment options");
					environmentRequested = false;
					return false;
				}
				constexpr wchar_t kCaptureBrowserArguments[] =
					L"--disable-backgrounding-occluded-windows --disable-renderer-backgrounding "
					L"--disable-features=CalculateNativeWinOcclusion,ApplyNativeOcclusionToCompositor";
				const auto optionsHr = environmentOptions->put_AdditionalBrowserArguments(
					kCaptureBrowserArguments);
				if (FAILED(optionsHr)) {
					log.Error(std::format(
						"WebView2 capture browser arguments rejected (0x{:08X})",
						static_cast<unsigned>(optionsHr)));
					environmentRequested = false;
					return false;
				}
				log.InfoFwd(
					"WebView2 renderer and native-occlusion throttling disabled for the offscreen capture host");
				const auto hr = ::CreateCoreWebView2EnvironmentWithOptions(
					nullptr, userData.c_str(), environmentOptions.Get(), callback.Get());
				if (FAILED(hr)) {
					log.Error(std::format("CreateCoreWebView2EnvironmentWithOptions failed (0x{:08X})",
						static_cast<unsigned>(hr)));
					// 0x80070002: the documented "runtime not found" result — a
					// missing or broken Evergreen install that slipped past the
					// startup version check.
					if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
						PromptInstallWebView2Runtime(log);
					}
					environmentRequested = false;
					return false;
				}
				return true;
			}

			void RequestController(View& a_view)
			{
				if (a_view.controllerRequested || !environment || !a_view.window) return;
				ComPtr<ICoreWebView2Environment3> environment3;
				if (FAILED(environment.As(&environment3))) {
					log.Error("composition controller API unavailable");
					return;
				}
				a_view.controllerRequested = true;
				// Capture the id, not the View*: the view can be destroyed while the
				// controller is still in flight.
				const auto hr = environment3->CreateCoreWebView2CompositionController(
					a_view.window,
					Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
						[this, id = a_view.id](HRESULT a_controllerHr,
							ICoreWebView2CompositionController* a_controller) -> HRESULT {
							if (auto* view = FindView(id)) {
								return OnController(*view, a_controllerHr, a_controller);
							}
							if (a_controller) {
								ComPtr<ICoreWebView2Controller> orphan;
								if (SUCCEEDED(ComPtr<ICoreWebView2CompositionController>(
										a_controller).As(&orphan))) {
									orphan->Close();
								}
							}
							return S_OK;
						}).Get());
				if (FAILED(hr)) {
					log.Error(std::format("view '{}': CreateCompositionController failed (0x{:08X})",
						a_view.id, static_cast<unsigned>(hr)));
					a_view.controllerRequested = false;
				}
			}

			// Window tree != process tree: parent this STA's host child under the
			// game's top-level window so Win32 focus/IME routing works, while the
			// browser processes stay outside the game's job/hooks. Runs once, on the
			// first controller success.
			void EnsureReparented()
			{
				if (reparented || !gameTopLevel) return;
				::SetLastError(ERROR_SUCCESS);
				const auto oldParent = ::SetParent(hostWindow, gameTopLevel);
				const auto parentError = ::GetLastError();
				if (!oldParent && parentError != ERROR_SUCCESS) {
					log.Error(std::format("cross-process SetParent failed ({})", parentError));
					return;
				}
				const auto style = static_cast<DWORD_PTR>(
					::GetWindowLongPtrW(hostWindow, GWL_STYLE));
				::SetWindowLongPtrW(hostWindow, GWL_STYLE,
					static_cast<LONG_PTR>((style & ~WS_POPUP) | WS_CHILD | WS_VISIBLE));
				::SetWindowPos(hostWindow, nullptr, 0, 0, 1, 1,
					SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_FRAMECHANGED);
				reparented = true;
				log.InfoFwd("host window reparented beneath the game window (cross-process)");
				if (bootstrapWindow) {
					::DestroyWindow(bootstrapWindow);
					bootstrapWindow = nullptr;
				}
			}

			HRESULT OnController(View& a_view, HRESULT a_hr,
				ICoreWebView2CompositionController* a_composition)
			{
				if (quit.load()) return S_OK;
				if (FAILED(a_hr) || !a_composition) {
					log.Error(std::format("view '{}': composition controller callback failed (0x{:08X})",
						a_view.id, static_cast<unsigned>(a_hr)));
					return S_OK;
				}
				a_view.compositionController = a_composition;
				EnsureReparented();

				if (FAILED(a_view.compositionController.As(&a_view.controller)) ||
					FAILED(a_view.controller->get_CoreWebView2(&a_view.webView)) || !a_view.webView) {
					log.Error(std::format("view '{}': failed to acquire CoreWebView2", a_view.id));
					return S_OK;
				}
				a_view.controller->put_Bounds(
					RECT{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) });
				ApplyScale(a_view);
				// Apply the current hidden state, except during the one-shot prewarm:
				// an invisible controller suspends Chromium rendering entirely.
				a_view.controller->put_IsVisible(
					a_view.hidden && !a_view.prewarmPending ? FALSE : TRUE);
				ComPtr<ICoreWebView2Controller2> controller2;
				if (SUCCEEDED(a_view.controller.As(&controller2))) {
					controller2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{ 0, 0, 0, 0 });
				}
				// The view's own child visual under the captured root; order and
				// visibility live on it.
				a_view.visual = compositor.CreateContainerVisual();
				a_view.visual.Size({ static_cast<float>(width), static_cast<float>(height) });
				a_view.visual.IsVisible(!a_view.hidden);
				ReorderVisuals();
				const auto target = a_view.visual.as<::IUnknown>();
				auto result = a_view.compositionController->put_RootVisualTarget(target.get());
				if (FAILED(result)) {
					log.Error(std::format("view '{}': put_RootVisualTarget failed (0x{:08X})",
						a_view.id, static_cast<unsigned>(result)));
					return S_OK;
				}
				ComPtr<ICoreWebView2_3> webView3;
				if (FAILED(a_view.webView.As(&webView3))) {
					log.Error("virtual host mapping API unavailable");
					return S_OK;
				}
				result = webView3->SetVirtualHostNameToFolderMapping(
					virtualHost.c_str(), viewsRoot.c_str(),
					COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
				if (FAILED(result)) {
					log.Error(std::format("SetVirtualHostNameToFolderMapping failed (0x{:08X})",
						static_cast<unsigned>(result)));
					return S_OK;
				}
				InstallNetworkGuard(a_view);
				InstallEvents(a_view);
				InstallBridgeShim(a_view);
				InstallRenderStats(a_view);
				// HUD-only mode leaves the widget OS-unfocused, and an unfocused renderer stops matching
				// :focus/:focus-visible/:focus-within and reports
				// document.hasFocus()=false — so focus styling (padnav's ring,
				// any view's own focus affordances) silently doesn't render even
				// though navigation works (2026-07-21 report: "arrows/gamepad
				// don't move focus"; they did — invisibly). Emulate a focused,
				// active page at the CDP layer. Interactive menus additionally get
				// real focus for their full session.
				a_view.webView->CallDevToolsProtocolMethod(
					L"Emulation.setFocusEmulationEnabled", LR"({"enabled":true})",
					Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
						[this, view = &a_view](HRESULT a_cdpHr, LPCWSTR) -> HRESULT {
							if (FAILED(a_cdpHr)) {
								log.Warn(std::format(
									"view '{}': focus emulation enable failed (0x{:08X}) — "
									"focus styling will not render without real focus",
									view->id, static_cast<unsigned>(a_cdpHr)));
							}
							return S_OK;
						}).Get());
				if (!captureStarted) {
					if (!StartCapture()) return S_OK;
					captureStarted = true;
					Send(json{ { "type", "ready" } });
				}
				log.InfoFwd(std::format("view '{}': controller ready ({} view(s) hosted)",
					a_view.id, views.size()));
				DrainQueuedViewWork(a_view);
				if (focusGranted && active == &a_view && !a_view.hidden) {
					a_view.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
				}
				ReconcileInputWidgetSubclass();
				return S_OK;
			}

			void InstallBridgeShim(View& a_view)
			{
				// Bridge contract: osfui.postMessage / osfui.onMessage, with
				// buffering for messages that arrive before onMessage is installed.
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

						// --- synthetic key delivery (__osfuiKey web messages) ---
						// Gamepad navigation reaches the page here as scripted
						// KeyboardEvents. padnav and the views key off keyCode / e.key,
						// not isTrusted, by contract.
						const VK_KEYS = {
							0x08: ['Backspace', 'Backspace'], 0x09: ['Tab', 'Tab'],
							0x0D: ['Enter', 'Enter'], 0x1B: ['Escape', 'Escape'],
							0x20: [' ', 'Space'],
							0x21: ['PageUp', 'PageUp'], 0x22: ['PageDown', 'PageDown'],
							0x23: ['End', 'End'], 0x24: ['Home', 'Home'],
							0x25: ['ArrowLeft', 'ArrowLeft'], 0x26: ['ArrowUp', 'ArrowUp'],
							0x27: ['ArrowRight', 'ArrowRight'], 0x28: ['ArrowDown', 'ArrowDown'],
							0x2D: ['Insert', 'Insert'], 0x2E: ['Delete', 'Delete'],
							0x10: ['Shift', 'ShiftLeft'], 0x11: ['Control', 'ControlLeft'],
							0x12: ['Alt', 'AltLeft'],
							0xA0: ['Shift', 'ShiftLeft'], 0xA1: ['Shift', 'ShiftRight'],
							0xA2: ['Control', 'ControlLeft'], 0xA3: ['Control', 'ControlRight'],
							0xA4: ['Alt', 'AltLeft'], 0xA5: ['Alt', 'AltRight']
						};
						const MOD_VKS = {
							0x10: 'shift', 0xA0: 'shift', 0xA1: 'shift',
							0x11: 'ctrl', 0xA2: 'ctrl', 0xA3: 'ctrl',
							0x12: 'alt', 0xA4: 'alt', 0xA5: 'alt'
						};
						const heldMods = { shift: false, ctrl: false, alt: false };
						const synthesizeKey = (vk, down) => {
							const mod = MOD_VKS[vk];
							if (mod) heldMods[mod] = down;
							let key, code;
							const named = VK_KEYS[vk];
							if (named) {
								key = named[0]; code = named[1];
							} else if (vk >= 0x41 && vk <= 0x5A) {
								const ch = String.fromCharCode(vk);
								key = heldMods.shift ? ch : ch.toLowerCase();
								code = 'Key' + ch;
							} else if (vk >= 0x30 && vk <= 0x39) {
								key = String.fromCharCode(vk);
								code = 'Digit' + key;
							} else if (vk >= 0x70 && vk <= 0x87) {
								key = 'F' + (vk - 0x6F); code = key;
							} else {
								return;  // unmapped VK: nothing sensible to synthesize
							}
							// Scripted dispatch does not cross frame boundaries the
							// way real (OS-focus) key routing does. If the focused
							// element is an <iframe> (e.g. a content mod embedding
							// its game in a frame), descend into same-origin frames
							// to the innermost focused document; hand cross-origin
							// frames the key via postMessage to the shim instance
							// injected there.
							let doc = document;
							let target = doc.activeElement || doc.body;
							while (target && target.tagName === 'IFRAME') {
								let inner = null;
								try { inner = target.contentDocument; } catch (_) {}
								if (!inner) break;
								doc = inner;
								target = doc.activeElement || doc.body;
							}
							if (target && target.tagName === 'IFRAME') {
								if (target.contentWindow)
									target.contentWindow.postMessage(
										{ __osfuiKeyRelay: { vk, down } }, '*');
								return;
							}
							if (!target) target = document.body;
							const ev = new KeyboardEvent(down ? 'keydown' : 'keyup', {
								key, code, bubbles: true, cancelable: true, composed: true,
								shiftKey: heldMods.shift, ctrlKey: heldMods.ctrl,
								altKey: heldMods.alt
							});
							// The constructor ignores legacy fields; padnav keys off
							// keyCode, so define them explicitly.
							Object.defineProperty(ev, 'keyCode', { get: () => vk });
							Object.defineProperty(ev, 'which', { get: () => vk });
							target.dispatchEvent(ev);
							// Scripted events run no native default actions. Emulate
							// the one the views rely on: arrow keys adjusting a
							// range slider (padnav deliberately leaves left/right
							// on a focused slider to the browser).
							if (down && !ev.defaultPrevented && target.tagName === 'INPUT' &&
								target.type === 'range' && key.startsWith('Arrow')) {
								const step = Number(target.step) || 1;
								const dir = (key === 'ArrowRight' || key === 'ArrowUp') ? 1 : -1;
								const min = target.min !== '' ? Number(target.min) : 0;
								const max = target.max !== '' ? Number(target.max) : 100;
								const cur = Number(target.value) || 0;
								const next = Math.min(max, Math.max(min, cur + dir * step));
								if (next !== cur) {
									target.value = String(next);
									target.dispatchEvent(new Event('input', { bubbles: true }));
									target.dispatchEvent(new Event('change', { bubbles: true }));
								}
							}
						};

						// Relay receiver: keys handed across a cross-origin frame
						// boundary by the parent's shim instance (synthesizeKey
						// above). Same-origin frames are reached by direct
						// dispatch and never see a relay.
						window.addEventListener('message', (event) => {
							if (event.source !== window.parent) return;
							const k = event.data && typeof event.data === 'object' ?
								event.data.__osfuiKeyRelay : null;
							if (k) synthesizeKey(k.vk | 0, !!k.down);
						});

						// Child frames may lack chrome.webview entirely; their shim
						// instance only serves the relay receiver above.
						if (!window.chrome || !chrome.webview) return;
						chrome.webview.addEventListener('message', (event) => {
							// Key channel: object-typed (PostWebMessageAsJson), never
							// forwarded to the page's onMessage.
							if (event.data && typeof event.data === 'object' &&
								event.data.__osfuiKey) {
								const k = event.data.__osfuiKey;
								synthesizeKey(k.vk | 0, !!k.down);
								return;
							}
							if (event.data && typeof event.data === 'object' &&
								event.data.__osfuiRenderStats) {
								if (typeof window.__osfuiRenderStatsNative === 'function')
									window.__osfuiRenderStatsNative(event.data.__osfuiRenderStats);
								return;
							}
							const json = typeof event.data === 'string' ?
								event.data : JSON.stringify(event.data);
							// Session boundary: on hide, blur any editable so the
							// runtime's cleared grant and the DOM agree — a field
							// left DOM-focused would otherwise look focused on
							// reopen but never re-grant (no new focusin).
							try {
								const m = JSON.parse(json);
								if (m && m.type === 'ui.visibility' && m.payload &&
									m.payload.visible === false) {
									const el = document.activeElement;
									if (el && el !== document.body && typeof el.blur === 'function') el.blur();
								}
							} catch (_) {}
							if (typeof onMessage === 'function') onMessage(json);
							else pending.push(json);
						});
					})();)JS";
				a_view.webView->AddScriptToExecuteOnDocumentCreated(shim,
					Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
						[this](HRESULT a_scriptHr, LPCWSTR) -> HRESULT {
							if (FAILED(a_scriptHr)) {
								log.Error(std::format("bridge shim install failed (0x{:08X})",
									static_cast<unsigned>(a_scriptHr)));
							}
							return S_OK;
						}).Get());
			}

			void InstallRenderStats(View& a_view)
			{
				// Shadow DOM isolates the host panel from arbitrary authored CSS. The
				// sampler is fully dormant while disabled and only writes twice a
				// second while enabled, keeping its own diagnostic cost bounded.
				static constexpr wchar_t script[] = LR"JS(
					(() => {
						let enabled=false, host=null, raf=0, lastFrame=0;
						let windowStart=0, frames=0, gaps=[], longCount=0, longMs=0;
						let observer=null, viewId='';
						const isTop=window===window.top;
						const safe=(n,d=1)=>Number.isFinite(Number(n))?Number(n).toFixed(d):'--';
						const ensurePanel=()=>{
							if(host&&host.isConnected)return host.shadowRoot;
							host=document.createElement('div');
							host.id='__osfui-render-stats';
							host.style.cssText='all:initial;position:fixed;z-index:2147483647;top:12px;right:12px;pointer-events:none;contain:layout style paint;';
							const root=host.attachShadow({mode:'open'});
							root.innerHTML=`<style>
								.panel{min-width:340px;padding:10px 12px;color:#e7f6ff;background:rgba(4,8,12,.92);border:1px solid rgba(118,199,239,.72);box-shadow:0 4px 20px rgba(0,0,0,.45);font:11px/1.45 ui-monospace,SFMono-Regular,Consolas,monospace;letter-spacing:.03em}
								.head{color:#7ed2ff;font-weight:700;border-bottom:1px solid rgba(126,210,255,.3);padding-bottom:5px;margin-bottom:5px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:360px}
								.row{display:grid;grid-template-columns:88px 1fr;gap:8px}.key{color:#8da0ad}.primary{color:#9ee6a8;font-weight:700}.bad{color:#ffb36b}
							</style><div class="panel"><div class="head"></div><div class="rows"></div></div>`;
							(document.documentElement||document).appendChild(host);
							return root;
						};
						const percentile=(values,p)=>{
							if(!values.length)return 0;
							const sorted=values.slice().sort((a,b)=>a-b);
							return sorted[Math.min(sorted.length-1,Math.floor(sorted.length*p))];
						};
						const frame=(now)=>{
							if(!enabled)return;
							if(!windowStart)windowStart=now;
							if(lastFrame)gaps.push(now-lastFrame);
							lastFrame=now;frames++;
							raf=requestAnimationFrame(frame);
						};
						const localSample=()=>{
							const now=performance.now(),elapsed=Math.max(1,now-windowStart);
							const result={
								pageFps:frames*1000/elapsed,p95:percentile(gaps,.95),
								max:gaps.length?Math.max(...gaps):0,longCount,longMs,
								nodes:document.getElementsByTagName('*').length,
								heapMb:performance.memory&&performance.memory.usedJSHeapSize?
									performance.memory.usedJSHeapSize/1048576:0,
								frame:location.pathname||document.title||'document'
							};
							windowStart=now;frames=0;gaps=[];longCount=0;longMs=0;
							return result;
						};
						const setChildStats=(next)=>{
							for(const iframe of document.querySelectorAll('iframe')){
								try{iframe.contentWindow?.__osfuiSetRenderStats?.(next,viewId);}catch(_){}
							}
						};
						const takeSample=()=>{
							let selected=localSample(),selectedArea=0;
							for(const iframe of document.querySelectorAll('iframe')){
								try{
									const rect=iframe.getBoundingClientRect();
									const area=Math.max(0,rect.width)*Math.max(0,rect.height);
									if(area<1)continue;
									const child=iframe.contentWindow;
									child?.__osfuiSetRenderStats?.(true,viewId);
									const sample=child?.__osfuiTakeRenderStatsSample?.();
									if(sample&&area>selectedArea){selected=sample;selectedArea=area;}
								}catch(_){}
							}
							return selected;
						};
						window.__osfuiTakeRenderStatsSample=takeSample;
						window.__osfuiRenderStatsNative=(native)=>{
							if(!enabled||!isTop)return;
							const page=takeSample();
							const {pageFps,p95,max,longCount,longMs}=page;
							const fresh=Number.isFinite(Number(native.freshFps))?native.freshFps:native.transferFps;
							const memory=page.heapMb?`${safe(page.heapMb,0)} MB`:'n/a';
							const root=ensurePanel(),bad=p95>25||max>50;
							root.querySelector('.head').textContent=`RENDER STATS · ${viewId}`;
							root.querySelector('.rows').innerHTML=
								`<div class="row primary"><span class="key">FRESH VIEW</span><span>${safe(fresh)} fps · new textures drawn in game</span></div>`+
								`<div class="row"><span class="key">OVERLAY PASSES</span><span>${safe(native.drawFps)} /s · ${native.reusedDraws||0} reused</span></div>`+
								`<div class="row"><span class="key">CAPTURE</span><span>${safe(native.captureFps)} fps WGC · publish ${safe(native.transferFps)}</span></div>`+
								`<div class="row"><span class="key">TRANSPORT</span><span>submit ${safe(native.submitFps)} fps · copy ${safe(native.copyMs,2)} ms</span></div>`+
								`<div class="row"><span class="key">LATENCY</span><span>capture→draw ${safe(native.sourceToDrawMs,2)} ms · CPU ${safe(native.recordCpuMs,3)} ms</span></div>`+
								`<div class="row ${bad?'bad':''}"><span class="key">PAGE RAF</span><span>${safe(pageFps)} fps · p95 ${safe(p95)} ms · max ${safe(max)} ms</span></div>`+
								`<div class="row ${longCount?'bad':''}"><span class="key">LONG TASKS</span><span>${longCount} · ${safe(longMs,0)} ms total</span></div>`+
								`<div class="row"><span class="key">PAGE</span><span>${page.nodes} nodes · heap ${memory}</span></div>`+
								`<div class="row ${native.backpressure||native.droppedBusy||native.skippedConcurrent?'bad':''}"><span class="key">DROPS/WAITS</span><span>ring ${native.backpressure||0} · GPU ${native.droppedBusy||0} · concurrent ${native.skippedConcurrent||0}</span></div>`;
							try{
								chrome.webview.postMessage('__osfuiRenderStatsPage:'+JSON.stringify({
									pageFps,p95,max,longCount,longMs,
									nodes:page.nodes,heapMb:page.heapMb,frame:page.frame
								}));
							}catch(_){}
						};
						window.__osfuiSetRenderStats=(next,id)=>{
							viewId=String(id||'');
							setChildStats(!!next);
							if(!!next===enabled)return;
							enabled=!!next;
							if(!enabled){
								if(raf)cancelAnimationFrame(raf);
								raf=0;lastFrame=0;windowStart=0;frames=0;gaps=[];
								if(observer)observer.disconnect();
								observer=null;if(host)host.remove();host=null;return;
							}
							if(isTop)ensurePanel();
							windowStart=performance.now();raf=requestAnimationFrame(frame);
							try{
								observer=new PerformanceObserver((list)=>{
									for(const entry of list.getEntries()){longCount++;longMs+=entry.duration;}
								});
								observer.observe({type:'longtask',buffered:true});
							}catch(_){}
						};
					})();
				)JS";
				a_view.webView->AddScriptToExecuteOnDocumentCreated(script,
					Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
						[this, view = &a_view](HRESULT a_hr, LPCWSTR) -> HRESULT {
							if (FAILED(a_hr)) {
								log.Warn(std::format(
									"view '{}': render-stats shim install failed (0x{:08X})",
									view->id, static_cast<unsigned>(a_hr)));
							}
							return S_OK;
						}).Get());
			}

			// Host of a URI, for the deny log / warn-once key only (not a parser).
			[[nodiscard]] static std::wstring UriHost(const std::wstring& a_uri)
			{
				const auto scheme = a_uri.find(L"://");
				if (scheme == std::wstring::npos) return a_uri;
				const auto start = scheme + 3;
				const auto end = a_uri.find_first_of(L"/?#", start);
				return a_uri.substr(start,
					end == std::wstring::npos ? std::wstring::npos : end - start);
			}

			// True only for the virtual-host origin the folder mapping serves.
			// The "/" (or end-of-string) right after the host is load-bearing:
			// it rejects https://osfui.local.evil.com/ and userinfo tricks like
			// https://osfui.local@evil.com/ without parsing the URI.
			[[nodiscard]] bool IsLocalViewUri(std::wstring a_uri) const
			{
				for (auto& ch : a_uri) {
					if (ch >= L'A' && ch <= L'Z') ch += 32;  // scheme/host are ASCII
				}
				for (const auto* scheme : { L"https://", L"http://" }) {
					const auto base = scheme + virtualHost;
					if (a_uri == base || a_uri.starts_with(base + L"/")) return true;
				}
				return false;
			}

			// security-model.md rule 2 (default-deny egress): everything a view may
			// legitimately load lives under the virtual-host folder mapping, so any
			// other destination is exfiltration surface. Two mechanisms, because no
			// single one covers everything:
			// - a WebResourceRequested filter answers non-local http(s) requests
			//   (documents, fetch/XHR, media, SSE; with source-kind ALL also
			//   service/shared-worker-initiated ones) locally with 403;
			// - WebResourceRequested cannot see non-HTTP transports, so the
			//   document-created script below removes their entry points
			//   (WebSocket, WebRTC, WebTransport) from every document instead.
			// Non-network schemes (about:, data:, blob:, devtools:) stay unfiltered.
			// Deliberate non-exceptions: devMode is NOT exempt (harness dev happens
			// in a desktop browser), and target=_blank links are unaffected — the
			// NewWindowRequested handler hands those to the OS default browser
			// without this WebView ever fetching them.
			void InstallNetworkGuard(View& a_view)
			{
				View* view = &a_view;
				HRESULT filterHr = E_NOINTERFACE;
				ComPtr<ICoreWebView2_22> webView22;
				if (SUCCEEDED(a_view.webView.As(&webView22))) {
					filterHr = S_OK;
					for (const auto* pattern : { L"http://*", L"https://*" }) {
						if (SUCCEEDED(filterHr)) {
							filterHr = webView22->AddWebResourceRequestedFilterWithRequestSourceKinds(
								pattern, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL,
								COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL);
						}
					}
				}
				if (FAILED(filterHr)) {
					// Pre-source-kinds Evergreen runtime: documents/fetch/XHR are
					// still filtered below; only requests initiated from service/
					// shared workers escape the older registration.
					log.Warn(std::format(
						"view '{}': source-kind egress filter unavailable (0x{:08X}); "
						"worker-initiated requests are not filtered on this WebView2 runtime",
						a_view.id, static_cast<unsigned>(filterHr)));
					filterHr = S_OK;
					for (const auto* pattern : { L"http://*", L"https://*" }) {
						if (SUCCEEDED(filterHr)) {
							filterHr = a_view.webView->AddWebResourceRequestedFilter(
								pattern, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
						}
					}
					if (FAILED(filterHr)) {
						log.Error(std::format(
							"view '{}': AddWebResourceRequestedFilter failed (0x{:08X}); "
							"network egress is NOT blocked",
							a_view.id, static_cast<unsigned>(filterHr)));
						return;
					}
				}
				EventRegistrationToken token{};
				a_view.webView->add_WebResourceRequested(
					Callback<ICoreWebView2WebResourceRequestedEventHandler>(
						[this, view](ICoreWebView2*,
							ICoreWebView2WebResourceRequestedEventArgs* a_args) -> HRESULT {
							ComPtr<ICoreWebView2WebResourceRequest> request;
							if (FAILED(a_args->get_Request(&request)) || !request) return S_OK;
							LPWSTR raw = nullptr;
							if (FAILED(request->get_Uri(&raw)) || !raw) return S_OK;
							std::wstring uri(raw);
							::CoTaskMemFree(raw);
							if (IsLocalViewUri(uri)) return S_OK;
							ComPtr<ICoreWebView2WebResourceResponse> response;
							if (environment && SUCCEEDED(environment->CreateWebResourceResponse(
									nullptr, 403, L"Forbidden", L"", &response))) {
								a_args->put_Response(response.Get());
							}
							const auto key = view->id + "|" + ToUtf8(UriHost(uri));
							if (egressWarned.insert(key).second) {
								log.Warn(std::format(
									"view '{}': blocked network egress to {} (further "
									"denials for this origin are silent)",
									view->id, ToUtf8(uri)));
							}
							return S_OK;
						}).Get(), &token);
				// The channels the request filter can't see. Runs in every document
				// (iframes included) before any page script; the non-configurable
				// define means page code cannot restore the constructor, and
				// `undefined` keeps feature detection on the graceful-degradation
				// path. Two groups:
				// - transports: WebSocket/WebRTC/WebTransport are non-HTTP, so the
				//   403 filter never sees them.
				// - workers: a Worker/SharedWorker loaded from a network URL gets its
				//   OWN CSP from its script response, not the document's, and this
				//   script does not run in worker scopes — so a worker is the one
				//   place the transport neutering above wouldn't reach. WebView2's
				//   folder mapping serves worker scripts internally without raising
				//   WebResourceRequested, so a per-response CSP header isn't an
				//   option either. Removing the constructors closes that scope
				//   entirely. Views are local, no-network mod UIs; none use workers.
				//   (Service workers are unaffected here but have no WebSocket, and
				//   their fetches are already caught by the request filter.)
				static constexpr auto kNeuter = LR"JS((() => {
					for (const name of ['WebSocket', 'RTCPeerConnection',
							'webkitRTCPeerConnection', 'WebTransport',
							'Worker', 'SharedWorker']) {
						try {
							Object.defineProperty(window, name,
								{ value: undefined, writable: false, configurable: false });
						} catch (_) {}
					}
				})();)JS";
				a_view.webView->AddScriptToExecuteOnDocumentCreated(kNeuter,
					Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
						[this](HRESULT a_scriptHr, LPCWSTR) -> HRESULT {
							if (FAILED(a_scriptHr)) {
								log.Error(std::format("egress neuter script install failed (0x{:08X})",
									static_cast<unsigned>(a_scriptHr)));
							}
							return S_OK;
						}).Get());
			}

			void InstallEvents(View& a_view)
			{
				// Views live behind stable unique_ptrs and their controllers are
				// Close()d before removal, so the raw View* in these callbacks cannot
				// outlive the view.
				View* view = &a_view;
				EventRegistrationToken token{};
				a_view.compositionController->add_CursorChanged(
					Callback<ICoreWebView2CursorChangedEventHandler>(
						[this, view](ICoreWebView2CompositionController* a_sender, ::IUnknown*) -> HRESULT {
							// Only the active view drives the real OS pointer.
							if (view != active) return S_OK;
							UINT32 id = 0;
							if (SUCCEEDED(a_sender->get_SystemCursorId(&id))) {
								Send(json{ { "type", "cursor" }, { "id", id } });
							}
							return S_OK;
						}).Get(), &token);
				a_view.controller->add_GotFocus(
					Callback<ICoreWebView2FocusChangedEventHandler>(
						[this](ICoreWebView2Controller*, ::IUnknown*) -> HRESULT {
							// Once the requested menu focus has landed, foreground mouse
							// capture is permitted and replaces Starfield's now-suspended
							// raw-input stream. An unsolicited focus grab outside a menu
							// session is still bounced back immediately.
							if (focusGranted) {
								ApplyMouseCapture();
								ReconcileInputWidgetSubclass();
							}
							if (!focusGranted && gameTopLevel) {
								::PostMessageW(gameTopLevel, kRestoreGameFocusMessage, 0, 0);
							}
							return S_OK;
						}).Get(), &token);
				a_view.controller->add_AcceleratorKeyPressed(
					Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
						[this](ICoreWebView2Controller*,
							ICoreWebView2AcceleratorKeyPressedEventArgs* a_args) -> HRESULT {
							UINT key = 0;
							COREWEBVIEW2_KEY_EVENT_KIND kind{};
							a_args->get_VirtualKey(&key);
							a_args->get_KeyEventKind(&kind);
							++accelEvents;
							const bool down =
								kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
								kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN;
							// Only physical presses arrive here; synthetic gamepad keys
							// are DOM events and never reach this hook.
							// Synchronous stand-in for Runtime::OnNativeAcceleratorKey;
							// the game keeps this state fresh over the pipe (accelState).
							const bool frameworkOwned =
								captureArmed ||
								(captureUpVk != 0 && key == captureUpVk) ||
								key == toggleVk ||
								(devReloadVk != 0 && key == devReloadVk) ||
								(key == 0x1B && captured);
							bool handled = down && handledKeys.contains(key);
							if (!handled && frameworkOwned) handled = true;
							if (frameworkOwned || (!down && handledKeys.contains(key))) {
								Send(json{ { "type", "accelerator" },
									{ "vk", key }, { "down", down } });
							}
							if (handled) {
								a_args->put_Handled(TRUE);
								if (down) handledKeys.insert(key);
							}
							if (!down) handledKeys.erase(key);
							return S_OK;
						}).Get(), &token);
				a_view.webView->add_WebMessageReceived(
					Callback<ICoreWebView2WebMessageReceivedEventHandler>(
						[this, view](ICoreWebView2*,
							ICoreWebView2WebMessageReceivedEventArgs* a_args) -> HRESULT {
							LPWSTR value = nullptr;
							if (FAILED(a_args->TryGetWebMessageAsString(&value)) || !value)
								return S_OK;
							auto text = ToUtf8(value);
							::CoTaskMemFree(value);
							if (text == kRevealSentinel) {
								// Host-internal paint handshake; not forwarded.
								CompleteReveal(*view, /*a_timedOut=*/false);
								return S_OK;
							}
							if (text == kPrewarmSentinel) {
								// Host-internal one-shot warmup; not forwarded.
								CompletePrewarm(*view);
								return S_OK;
							}
							static constexpr std::string_view kStatsPrefix = "__osfuiRenderStatsPage:";
							if (text.starts_with(kStatsPrefix)) {
								const auto now = ::GetTickCount64();
								if (view->renderStats &&
									(now - view->renderStatsLastPageLogMs >= 2000)) {
									const auto sample = json::parse(text.substr(kStatsPrefix.size()), nullptr, false);
									if (!sample.is_discarded()) {
										try {
											view->renderStatsLastPageLogMs = now;
											log.InfoFwd(std::format(
												"view '{}': page diagnostics for '{}': RAF {:.1f} fps, gap p95 {:.2f} ms, "
												"max {:.2f} ms, long tasks {} / {:.1f} ms, DOM {} nodes, heap {:.1f} MB",
												view->id, sample.value("frame", std::string{ "document" }),
												sample.value("pageFps", 0.0), sample.value("p95", 0.0),
												sample.value("max", 0.0), sample.value("longCount", 0ull),
												sample.value("longMs", 0.0), sample.value("nodes", 0ull),
												sample.value("heapMb", 0.0)));
										} catch (...) {
											// Authored content can post arbitrary strings; malformed
											// lookalikes must not enter the public bridge or fault the host.
										}
									}
								}
								return S_OK;  // host-internal diagnostics; never enter the public bridge
							}
							Send(json{ { "type", "webMessage" }, { "view", view->id },
								{ "json", std::move(text) } });
							return S_OK;
						}).Get(), &token);
				a_view.webView->add_NewWindowRequested(
					Callback<ICoreWebView2NewWindowRequestedEventHandler>(
						[this, view](ICoreWebView2*,
							ICoreWebView2NewWindowRequestedEventArgs* a_args) -> HRESULT {
							// Views are local content; a target="_blank" link (e.g. the
							// settings view's "needs update" tag pointing at Nexus) means
							// "leave the game". Unhandled, WebView2 would spawn a popup
							// window over the game instead.
							a_args->put_Handled(TRUE);
							LPWSTR raw = nullptr;
							if (FAILED(a_args->get_Uri(&raw)) || !raw) return S_OK;
							std::wstring uri(raw);
							::CoTaskMemFree(raw);
							if (!uri.starts_with(L"https://") && !uri.starts_with(L"http://")) {
								log.Warn(std::format("view '{}': blocked non-http new-window: {}",
									view->id, ToUtf8(uri)));
								return S_OK;
							}
							const auto rc = reinterpret_cast<INT_PTR>(::ShellExecuteW(
								nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
							if (rc <= 32) {
								log.Error(std::format("view '{}': browser open failed ({}): {}",
									view->id, rc, ToUtf8(uri)));
							} else {
								log.InfoFwd(std::format("view '{}': opened in default browser: {}",
									view->id, ToUtf8(uri)));
							}
							return S_OK;
						}).Get(), &token);
				a_view.webView->add_NavigationCompleted(
					Callback<ICoreWebView2NavigationCompletedEventHandler>(
						[this, view](ICoreWebView2*,
							ICoreWebView2NavigationCompletedEventArgs* a_args) -> HRESULT {
							BOOL success = FALSE;
							COREWEBVIEW2_WEB_ERROR_STATUS status{};
							a_args->get_IsSuccess(&success);
							a_args->get_WebErrorStatus(&status);
							Send(json{ { "type", "loadEvent" },
								{ "view", view->id },
								{ "failed", success != TRUE },
								{ "url", ToUtf8(view->currentUrl) },
								{ "description", success ? "" : "WebView2 navigation failed" },
								{ "code", static_cast<int>(status) } });
							view->navigationSucceeded = success == TRUE;
							if (!view->navigationSucceeded && view->prewarmPending) {
								CompletePrewarm(*view);
							}
							if (view->navigationSucceeded && view->domSeen && !view->domNotified) {
								view->domNotified = true;
								Send(json{ { "type", "domReady" }, { "view", view->id } });
							}
							return S_OK;
						}).Get(), &token);
				ComPtr<ICoreWebView2_2> webView2;
				if (SUCCEEDED(a_view.webView.As(&webView2))) {
					webView2->add_DOMContentLoaded(
						Callback<ICoreWebView2DOMContentLoadedEventHandler>(
							[this, view](ICoreWebView2*, ICoreWebView2DOMContentLoadedEventArgs*) -> HRESULT {
								view->domSeen = true;
								if (view->navigationSucceeded && !view->domNotified) {
									view->domNotified = true;
									Send(json{ { "type", "domReady" }, { "view", view->id } });
								}
								DrainQueuedViewWork(*view);
								ApplyRenderStats(*view);
								BeginPrewarm(*view);
								return S_OK;
							}).Get(), &token);
				}
				a_view.webView->add_ProcessFailed(
					Callback<ICoreWebView2ProcessFailedEventHandler>(
						[this, view](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* a_args) -> HRESULT {
							COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
							a_args->get_ProcessFailedKind(&kind);
							log.Error(std::format("view '{}': browser process failed (kind {})",
								view->id, static_cast<int>(kind)));
							switch (kind) {
							case COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED:
								// The whole environment died with the browser process;
								// every controller is dead COM and a Navigate cannot
								// revive anything. Exit so the pipe drop reaches the
								// game's host-death path (overlay hidden, logged)
								// instead of leaving a zombie host the game still
								// believes in.
								byeReason = "browser-process-exited";
								quit.store(true);
								break;
							case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED:
							case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE:
								// This view's content is gone/hung but the webView
								// object survives, and a Navigate revives it — report
								// a failed load so the game's crash-recovery reload
								// reacts (Runtime::OnViewLoad) rather than leaving a
								// blank input-capturing shell.
								view->navigationSucceeded = false;
								view->domSeen = false;
								view->domNotified = false;
								Send(json{ { "type", "loadEvent" },
									{ "view", view->id },
									{ "failed", true },
									{ "url", ToUtf8(view->currentUrl) },
									{ "description",
										kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED
											? "WebView2 render process exited"
											: "WebView2 render process unresponsive" },
									{ "code", static_cast<int>(kind) } });
								break;
							default:
								// Frame/GPU/utility children restart on their own.
								break;
							}
							return S_OK;
						}).Get(), &token);
				// devMode only: the game side registers a console handler solely in
				// devMode, so forwarding in release would cross the pipe just to be
				// dropped (and Runtime.enable keeps DevTools instrumentation live in
				// every renderer for nothing).
				if (devMode &&
					SUCCEEDED(a_view.webView->GetDevToolsProtocolEventReceiver(
						L"Runtime.consoleAPICalled", &a_view.consoleReceiver)) &&
					a_view.consoleReceiver) {
					a_view.consoleReceiver->add_DevToolsProtocolEventReceived(
						Callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
							[this, view](ICoreWebView2*,
								ICoreWebView2DevToolsProtocolEventReceivedEventArgs* a_args) -> HRESULT {
								LPWSTR value = nullptr;
								if (SUCCEEDED(a_args->get_ParameterObjectAsJson(&value)) && value) {
									Send(json{ { "type", "console" }, { "view", view->id },
										{ "json", ToUtf8(value) } });
									::CoTaskMemFree(value);
								}
								return S_OK;
							}).Get(), &token);
					a_view.webView->CallDevToolsProtocolMethod(L"Runtime.enable", L"{}",
						Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
							[](HRESULT, LPCWSTR) -> HRESULT { return S_OK; }).Get());
				}
			}

			void ApplyCaptureCadence()
			{
				if (!captureSession) return;
				const std::uint32_t desiredHz = focusGranted ? 240u : 60u;
				if (captureCadenceHz == desiredHz) return;
				try {
					if (const auto cadence = captureSession.try_as<
						winrt::Windows::Graphics::Capture::IGraphicsCaptureSession5>()) {
						const auto interval = std::chrono::duration_cast<
							winrt::Windows::Foundation::TimeSpan>(
								std::chrono::duration<double>(
									1.0 / static_cast<double>(desiredHz)));
						cadence.MinUpdateInterval(interval);
						const auto applied = cadence.MinUpdateInterval();
						captureCadenceHz = desiredHz;
						log.Info(std::format(
							"WGC cadence -> {} mode, up to {} Hz ({:.3f} ms)",
							focusGranted ? "interactive" : "HUD",
							desiredHz,
							std::chrono::duration<double, std::milli>(applied).count()));
					} else {
						captureCadenceHz = desiredHz;
						log.Info("WGC explicit cadence control unavailable; using the system default");
					}
				} catch (const winrt::hresult_error& a_error) {
					log.Warn(std::format("WGC cadence update failed: {}", ToUtf8(a_error.message())));
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
						[this](winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& a_pool,
							winrt::Windows::Foundation::IInspectable const&) {
							OnFrameArrived(a_pool);
						});
					captureSession = framePool.CreateCaptureSession(captureItem);
					try { captureSession.IsCursorCaptureEnabled(false); } catch (...) {}
					// Interactive menus get a permissive ceiling so capture can follow a
					// high-refresh foreground WebView. HUD-only mode is deliberately capped
					// at 60 Hz to bound capture/copy pressure while gameplay owns the GPU.
					ApplyCaptureCadence();
					captureSession.StartCapture();
					return true;
				} catch (const winrt::hresult_error& a_error) {
					log.Error(std::format("capture setup failed: {} (0x{:08X})",
						ToUtf8(a_error.message()), static_cast<unsigned>(a_error.code())));
					return false;
				}
			}

			void OnFrameArrived(
				const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& a_pool)
			{
				if (quit.load() || captureClosing.load()) return;
				try {
					const auto arrival = std::chrono::steady_clock::now();
					if (AnyVisibleRenderStats() && captureLastArrival.time_since_epoch().count() != 0) {
						const auto gapMs = std::chrono::duration<double, std::milli>(
							arrival - captureLastArrival).count();
						// Gaps over a second are idle pauses (nothing painted),
						// not cadence, and would swamp the average.
						if (gapMs < 1000.0) {
							captureGapMsTotal += gapMs;
							captureGapMsMin = captureGapCount == 0 ? gapMs : (std::min)(captureGapMsMin, gapMs);
							captureGapMsMax = captureGapCount == 0 ? gapMs : (std::max)(captureGapMsMax, gapMs);
							++captureGapCount;
							if (captureGapCount % 600 == 0) {
								log.Info(std::format(
									"capture cadence: {} gaps, avg {:.2f} ms ({:.1f}/s), min {:.2f}, max {:.2f}",
									captureGapCount, captureGapMsTotal / static_cast<double>(captureGapCount),
									1000.0 * static_cast<double>(captureGapCount) / captureGapMsTotal,
									captureGapMsMin, captureGapMsMax));
								captureGapMsTotal = 0.0;
								captureGapCount = 0;
							}
						}
					}
					captureLastArrival = arrival;
					auto capturedFrame = a_pool.TryGetNextFrame();
					if (!capturedFrame) return;
					captureArrivalCount.fetch_add(1, std::memory_order_relaxed);
					auto access = capturedFrame.Surface().as<
						::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
					ComPtr<ID3D11Texture2D> source;
					winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&source)));
					D3D11_TEXTURE2D_DESC desc{};
					source->GetDesc(&desc);
					// No warmup drop: a static page may paint fewer than 3 times in
					// total, so the first captured frame has to publish.
					PublishFrame(source.Get(), desc.Width, desc.Height);
				} catch (const winrt::hresult_error& a_error) {
					log.Warn(std::format("capture callback failed: {}", ToUtf8(a_error.message())));
				}
			}

			// Command handling; STA thread.

			void DrainQueuedViewWork(View& a_view)
			{
				if (!a_view.webView) return;
				if (a_view.pendingNavigate) {
					a_view.domSeen = a_view.navigationSucceeded = a_view.domNotified = false;
					a_view.prewarmPending = false;
					a_view.prewarmDeadline = 0;
					a_view.currentUrl = *a_view.pendingNavigate;
					a_view.pendingNavigate.reset();
					if (a_view.prewarm && a_view.hidden) BeginPrewarm(a_view);
					const auto hr = a_view.webView->Navigate(a_view.currentUrl.c_str());
					if (FAILED(hr)) {
						CompletePrewarm(a_view);
						Send(json{ { "type", "loadEvent" }, { "view", a_view.id },
							{ "failed", true },
							{ "url", ToUtf8(a_view.currentUrl) },
							{ "description", "Navigate returned failure" },
							{ "code", static_cast<int>(hr) } });
					}
				}
				if (!a_view.domSeen) return;
				for (auto& message : a_view.queuedPostWeb) {
					const auto wide = ToWide(message);
					a_view.webView->PostWebMessageAsString(wide.c_str());
				}
				a_view.queuedPostWeb.clear();
				for (auto& eval : a_view.queuedEvals) {
					RunEval(a_view, eval.id, eval.script);
				}
				a_view.queuedEvals.clear();
			}

			void RunEval(View& a_view, std::uint64_t a_id, const std::string& a_script)
			{
				const auto wide = ToWide(a_script);
				a_view.webView->ExecuteScript(wide.c_str(),
					Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
						[this, a_id](HRESULT a_hr, LPCWSTR a_json) -> HRESULT {
							Send(json{ { "type", "evalResult" }, { "id", a_id },
								{ "result", SUCCEEDED(a_hr) && a_json ?
									ToUtf8(a_json) : std::string{} } });
							return S_OK;
						}).Get());
			}

			// Bounds are physical pixels (always the output size, so the composited
			// stack maps 1:1); the rasterization scale is what makes the page lay out
			// at its manifest height and scales CSS px up to output pixels. Without
			// it a view lays out at scale 1.0 against the full output resolution,
			// i.e. undersized on any display taller than the manifest (visibly so at
			// 1440p/4K). ShouldDetectMonitorScaleChanges must be off, or WebView2
			// folds the monitor's DPI in on top of ours and the result becomes
			// machine-dependent.
			void ApplyScale(View& a_view)
			{
				if (!a_view.controller) return;
				ComPtr<ICoreWebView2Controller4> controller4;
				if (FAILED(a_view.controller.As(&controller4)) || !controller4) {
					// Pre-1.0.1108 runtime: no rasterization scale to set, so the page
					// renders unscaled. Logged once per view so an odd-looking overlay
					// is traceable.
					log.Warn(std::format("view '{}': ICoreWebView2Controller4 unavailable — "
						"rasterization scale not applied (WebView2 runtime too old)", a_view.id));
					return;
				}
				controller4->put_ShouldDetectMonitorScaleChanges(FALSE);
				const auto logical = (std::max)(1u, a_view.logicalHeight);
				controller4->put_RasterizationScale(
					static_cast<double>(height) / static_cast<double>(logical));
			}

			void ApplyResize(std::uint32_t a_width, std::uint32_t a_height)
			{
				width = (std::max)(1u, a_width);
				height = (std::max)(1u, a_height);
				if (rootVisual) {
					rootVisual.Size({ static_cast<float>(width), static_cast<float>(height) });
				}
				// Every view renders output-sized so the composited stack maps 1:1.
				for (auto& view : views) {
					if (view->visual) {
						view->visual.Size({ static_cast<float>(width), static_cast<float>(height) });
					}
					if (view->controller) {
						view->controller->put_Bounds(
							RECT{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) });
						ApplyScale(*view);
					}
				}
				if (!framePool) return;
				try {
					framePool.Recreate(captureDevice,
						winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
						3, winrt::Windows::Graphics::SizeInt32{
							static_cast<std::int32_t>(width), static_cast<std::int32_t>(height) });
				} catch (const winrt::hresult_error& a_error) {
					log.Warn(std::format("frame pool resize failed: {}", ToUtf8(a_error.message())));
				}
				// The ring recreates lazily on the next capture at the new dimensions
				// (PublishFrame -> EnsureRing).
			}

			/// The active view's Chromium widget: the HWND that holds keyboard
			/// focus during an interactive-menu session. The session input path
			/// subclasses it for wheel/rebind messages (synthetic keys are DOM events).
			HWND FindActiveWidget() const
			{
				HWND widget = ::GetFocus();
				if (active && active->window && widget &&
					(widget == active->window || ::IsChild(active->window, widget))) {
					return widget;
				}
				widget = nullptr;
				if (active && active->window) {
					::EnumChildWindows(active->window, [](HWND a_hwnd, LPARAM a_param) -> BOOL {
						wchar_t name[128]{};
						::GetClassNameW(a_hwnd, name, static_cast<int>(std::size(name)));
						if (std::wstring_view(name).starts_with(L"Chrome_WidgetWin_")) {
							*reinterpret_cast<HWND*>(a_param) = a_hwnd;
							return FALSE;
						}
						return TRUE;
					}, reinterpret_cast<LPARAM>(&widget));
				}
				return widget;
			}

			/// Keeps one subclass while either native menu focus needs physical
			/// wheel routing or key-rebind capture needs character-key interception.
			/// Idempotent and safe across view switches/teardown.
			void ReconcileInputWidgetSubclass()
			{
				if (captureArmed || focusGranted) {
					HWND widget = FindActiveWidget();
					if (!widget || widget == captureWidget) return;
					RemoveCaptureSubclass();  // a different view is active now
					s_app = this;             // before install: the proc may run immediately
					auto* previous = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
						widget, GWLP_WNDPROC,
						reinterpret_cast<LONG_PTR>(&CaptureWndProc)));
					if (!previous) {
						s_app = nullptr;  // subclass refused; accelerators still work
						return;
					}
					captureWidget = widget;
					captureWidgetProc = previous;
				} else {
					RemoveCaptureSubclass();
				}
			}

			[[nodiscard]] bool SendFocusedMouseWheel(WPARAM a_wparam)
			{
				if (!focusGranted || !active || !active->compositionController) {
					return false;
				}
				const auto delta = static_cast<SHORT>(HIWORD(a_wparam));
				if (delta == 0) return false;
				active->compositionController->SendMouseInput(
					COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL,
					static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(
						static_cast<UINT32>(LOWORD(a_wparam))),
					static_cast<UINT32>(delta),
					POINT{ capturedMouseX, capturedMouseY });
				return true;
			}

			void RemoveCaptureSubclass()
			{
				if (!captureWidget) return;
				// Only unhook if we are still the installed proc: restoring blindly
				// over someone else's later subclass would strand it.
				const auto current = reinterpret_cast<WNDPROC>(
					::GetWindowLongPtrW(captureWidget, GWLP_WNDPROC));
				if (current == &CaptureWndProc && captureWidgetProc) {
					::SetWindowLongPtrW(captureWidget, GWLP_WNDPROC,
						reinterpret_cast<LONG_PTR>(captureWidgetProc));
				}
				captureWidget = nullptr;
				captureWidgetProc = nullptr;
				s_app = nullptr;  // after the restore above, never before
			}

			/// Runs on the host's UI thread (the widget's own thread, same one as the
			/// message pump), so it touches app state directly.
			static LRESULT CALLBACK CaptureWndProc(
				HWND a_hwnd, UINT a_msg, WPARAM a_wparam, LPARAM a_lparam)
			{
				auto* self = s_app;
				if (self && a_msg == WM_MOUSEWHEEL &&
					self->SendFocusedMouseWheel(a_wparam)) {
					// Wheel messages target the keyboard-focused Chromium widget,
					// not the HWND holding SetCapture. Forward explicitly into the
					// windowless composition controller and suppress duplication.
					return 0;
				}
				if (self && self->captureArmed &&
					(a_msg == WM_KEYDOWN || a_msg == WM_SYSKEYDOWN)) {
					const auto vk = static_cast<std::uint32_t>(a_wparam);
					const bool repeat = (a_lparam & 0x40000000) != 0;
					if (!repeat) {
						// Same envelope as the accelerator path, so the game side
						// needs no new message type. Swallowed: mid-rebind the
						// press is a binding, not text for the page.
						self->Send(json{ { "type", "accelerator" },
							{ "vk", vk }, { "down", true } });
						return 0;
					}
				}
				const auto proc = (self && self->captureWidgetProc)
					? self->captureWidgetProc
					: nullptr;
				return proc ? ::CallWindowProcW(proc, a_hwnd, a_msg, a_wparam, a_lparam)
							: ::DefWindowProcW(a_hwnd, a_msg, a_wparam, a_lparam);
			}

			void ApplyMouseCapture()
			{
				if (focusGranted && hostWindow) {
					if (::GetCapture() != hostWindow) {
						::SetCapture(hostWindow);
					}
				} else {
					if (hostWindow && ::GetCapture() == hostWindow) {
						::ReleaseCapture();
					}
				}
			}

			[[nodiscard]] bool SendCapturedMouse(
				UINT a_msg, WPARAM a_wparam, LPARAM a_lparam)
			{
				if (!focusGranted || !active || !active->compositionController ||
					!hostWindow || !gameTopLevel) {
					return false;
				}

				POINT point{
					static_cast<SHORT>(LOWORD(a_lparam)),
					static_cast<SHORT>(HIWORD(a_lparam))
				};
				// Wheel messages carry screen coordinates; the other legacy mouse
				// messages are relative to our captured 1x1 host HWND.
				if (a_msg != WM_MOUSEWHEEL) {
					::ClientToScreen(hostWindow, &point);
				}
				::ScreenToClient(gameTopLevel, &point);
				RECT client{};
				if (!::GetClientRect(gameTopLevel, &client) ||
					client.right <= client.left || client.bottom <= client.top) {
					return false;
				}
				const auto x = std::clamp(::MulDiv(point.x - client.left,
					static_cast<int>(width), client.right - client.left),
					0, static_cast<int>(width) - 1);
				const auto y = std::clamp(::MulDiv(point.y - client.top,
					static_cast<int>(height), client.bottom - client.top),
					0, static_cast<int>(height) - 1);
				capturedMouseX = x;
				capturedMouseY = y;

				COREWEBVIEW2_MOUSE_EVENT_KIND eventKind{};
				switch (a_msg) {
				case WM_MOUSEMOVE:   eventKind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE; break;
				case WM_LBUTTONDOWN: eventKind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN; break;
				case WM_LBUTTONUP:   eventKind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP; break;
				case WM_RBUTTONDOWN: eventKind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN; break;
				case WM_RBUTTONUP:   eventKind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP; break;
				case WM_MBUTTONDOWN: eventKind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN; break;
				case WM_MBUTTONUP:   eventKind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP; break;
				case WM_MOUSEWHEEL:  eventKind = COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL; break;
				default: return false;
				}
				const auto keys = static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(
					static_cast<UINT32>(LOWORD(a_wparam)));
				const auto data = a_msg == WM_MOUSEWHEEL ?
					static_cast<UINT32>(static_cast<SHORT>(HIWORD(a_wparam))) : 0u;
				active->compositionController->SendMouseInput(
					eventKind, keys, data, POINT{ x, y });
				return true;
			}

			static LRESULT CALLBACK HostInputWndProc(
				HWND a_hwnd, UINT a_msg, WPARAM a_wparam, LPARAM a_lparam)
			{
				auto* self = s_hostInputApp;
				if (self && self->focusGranted) {
					if (a_msg == WM_SETCURSOR) {
						// While this HWND owns capture, the game no longer receives
						// mouse packets on which to apply CursorChanged. Apply the
						// composition controller's current CSS system cursor here.
						UINT32 id = 0;
						if (self->active && self->active->compositionController &&
							SUCCEEDED(self->active->compositionController->get_SystemCursorId(&id))) {
							HCURSOR cursor = id == 0 ? nullptr : ::LoadCursorW(
								nullptr, MAKEINTRESOURCEW(id));
							if (id != 0 && !cursor) {
								cursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
							}
							::SetCursor(cursor);
						}
						return TRUE;
					}
					if (self->SendCapturedMouse(a_msg, a_wparam, a_lparam)) {
						return 0;
					}
				}
				const auto proc = self ? self->hostWindowProc : nullptr;
				return proc ? ::CallWindowProcW(proc, a_hwnd, a_msg, a_wparam, a_lparam)
							: ::DefWindowProcW(a_hwnd, a_msg, a_wparam, a_lparam);
			}

			void SendMouse(const json& a_msg)
			{
				// Mouse always targets the active view: the runtime routes input to
				// the top menu, so sibling views never see the pointer.
				if (!active || !active->compositionController) return;
				const std::string kind = a_msg.value("kind", "move");
				int x = a_msg.value("x", 0);
				int y = a_msg.value("y", 0);
				// Right-stick scrolling still arrives over the pipe. Once the host
				// owns physical mouse capture, target it at the last real pointer
				// position rather than the game runtime's now-stale WM_INPUT position.
				if (focusGranted && kind == "wheel") {
					x = capturedMouseX;
					y = capturedMouseY;
				}
				if (kind == "move") {
					capturedMouseX = std::clamp(x, 0, static_cast<int>(width) - 1);
					capturedMouseY = std::clamp(y, 0, static_cast<int>(height) - 1);
				}
				COREWEBVIEW2_MOUSE_EVENT_KIND eventKind{};
				UINT32 data = 0;
				if (kind == "move") {
					eventKind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE;
				} else if (kind == "wheel") {
					eventKind = COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL;
					data = static_cast<UINT32>(a_msg.value("wheel", 0));
				} else {
					const int  button = a_msg.value("button", 0);
					const bool down = a_msg.value("down", false);
					if (button == 0) {
						eventKind = down ? COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN :
							COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
					} else if (button == 1) {
						eventKind = down ? COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN :
							COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP;
					} else {
						eventKind = down ? COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN :
							COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP;
					}
				}
				active->compositionController->SendMouseInput(eventKind,
					COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, data, POINT{ x, y });
			}

			void HandleCommand(const json& a_msg)
			{
				const std::string type = a_msg.value("type", "");
				if (type == "init") {
					if (initialized) return;
					initialized = true;
					gameTopLevel = reinterpret_cast<HWND>(
						static_cast<std::uintptr_t>(a_msg.value("topLevelHwnd", 0ull)));
					viewsRoot = std::filesystem::path(ToWide(a_msg.value("viewsPath", "")));
					virtualHost = ToWide(a_msg.value("virtualHost", "osfui.local"));
					width = (std::max)(1u, a_msg.value("width", 1u));
					height = (std::max)(1u, a_msg.value("height", 1u));
					userData = std::filesystem::path(ToWide(a_msg.value("userDataDir", "")));
					devMode = a_msg.value("devMode", false);
					defaultHidden = a_msg.value("hidden", true);
					if (userData.empty()) {
						log.Error("init without userDataDir");
						return;
					}
					std::optional<LUID> requestedAdapter;
					if (a_msg.contains("adapterLuidLow") && a_msg.contains("adapterLuidHigh")) {
						LUID luid{};
						luid.LowPart = a_msg.value("adapterLuidLow", 0u);
						luid.HighPart = static_cast<LONG>(a_msg.value("adapterLuidHigh", 0u));
						requestedAdapter = luid;
					}
					if (!InitializeGraphics(requestedAdapter)) {
						byeReason = "graphics-init-failed";
						quit.store(true);
						return;
					}
					rootVisual.Size({ static_cast<float>(width), static_cast<float>(height) });
					log.Info(std::format("init: views='{}' {}x{} hidden={} topLevel=0x{:X}",
						ToUtf8(viewsRoot.native()), width, height, defaultHidden,
						reinterpret_cast<std::uintptr_t>(gameTopLevel)));
					BeginEnvironment();
				} else if (type == "navigate") {
					// `id` is the view id; the first navigate for an unknown id
					// creates that view.
					const std::string id = a_msg.value("id", "");
					if (id.empty()) {
						log.Warn("navigate without id ignored");
						return;
					}
					auto* view = FindView(id);
					if (!view) view = &CreateView(id);
					view->logicalHeight = (std::max)(1u,
						a_msg.value("logicalHeight", kDefaultLogicalHeight));
					// A re-navigate may carry a different manifest height (dev
					// reload) onto an existing controller, so re-apply.
					ApplyScale(*view);
					std::string path = id + "/" + a_msg.value("entry", "index.html");
					std::ranges::replace(path, '\\', '/');
					view->pendingNavigate = L"https://" + virtualHost + L"/" + ToWide(path);
					if (view->webView) {
						DrainQueuedViewWork(*view);
					} else {
						RequestController(*view);  // no-op until the environment is up
					}
				} else if (type == "resize") {
					ApplyResize(a_msg.value("width", 1u), a_msg.value("height", 1u));
				} else if (type == "prewarm") {
					auto* view = ResolveView(a_msg);
					if (!view) return;
					view->prewarm = true;
					BeginPrewarm(*view);
				} else if (type == "setHidden") {
					auto* view = ResolveView(a_msg);
					if (!view) return;
					if (a_msg.value("hidden", true)) {
						HideView(*view);
					} else {
						ShowView(*view);
					}
				} else if (type == "setOrder") {
					auto* view = ResolveView(a_msg);
					if (!view) return;
					view->order = a_msg.value("order", 0);
					ReorderVisuals();
				} else if (type == "setRenderStats") {
					auto* view = ResolveView(a_msg);
					if (!view) return;
					view->renderStats = a_msg.value("enabled", false);
					ApplyRenderStats(*view);
				} else if (type == "renderStatsSample") {
					compositorStats = a_msg;
					compositorStats.erase("type");
				} else if (type == "setActive") {
					auto* view = ResolveView(a_msg);
					if (view) {
						active = view;
						log.Info(std::format("active view -> '{}'", view->id));
						if (focusGranted && view->controller && !view->hidden) {
							view->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
						}
						ReconcileInputWidgetSubclass();
					}
				} else if (type == "focus") {
					// Session focus: true for an input-capturing menu, false for
					// HUD-only/closed states. Revoke releases mouse capture; the
					// game side restores its own focus on its window thread.
					const bool focused = a_msg.value("focused", false);
					focusGranted = focused;
					if (focused && active && active->controller && !active->hidden) {
						active->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
					}
					ReconcileInputWidgetSubclass();
					ApplyMouseCapture();
					ApplyCaptureCadence();
				} else if (type == "mouse") {
					SendMouse(a_msg);
				} else if (type == "key") {
					// Synthetic keys (gamepad nav taps, Esc back-delegation, and
					// keyboard keys the game WndProc swallows while the game owns
					// focus). Delivered as DOM KeyboardEvents by the bridge shim
					// (`__osfuiKey` web message), NOT as Win32 key messages:
					// Chromium ignores WM_KEYDOWN posted to an unfocused widget.
					// The object-typed message is
					// distinguishable from runtime traffic, which arrives as
					// strings (PostWebMessageAsString).
					const auto vk = a_msg.value("vk", 0u);
					const bool down = a_msg.value("down", false);
					if (active && active->webView) {
						const auto payload = json{ { "__osfuiKey",
							{ { "vk", vk }, { "down", down } } } }.dump();
						active->webView->PostWebMessageAsJson(ToWide(payload).c_str());
					}
				} else if (type == "frameAck") {
					// Monotonic release marker for frames the game never read.
					const auto serial = a_msg.value("serial", 0ull);
					auto current = ackedSerial.load();
					while (serial > current &&
						   !ackedSerial.compare_exchange_weak(current, serial)) {}
				} else if (type == "postWeb") {
					if (auto* view = ResolveView(a_msg)) {
						view->queuedPostWeb.push_back(a_msg.value("json", ""));
						DrainQueuedViewWork(*view);
					}
				} else if (type == "eval") {
					if (auto* view = ResolveView(a_msg)) {
						view->queuedEvals.push_back(
							{ a_msg.value("id", 0ull), a_msg.value("script", "") });
						DrainQueuedViewWork(*view);
					}
				} else if (type == "accelState") {
					const bool wasCaptured = captured;
					toggleVk = a_msg.value("toggleVk", 0u);
					devReloadVk = a_msg.value("devReloadVk", 0u);
					captured = a_msg.value("captured", false);
					captureArmed = a_msg.value("captureArmed", false);
					captureUpVk = a_msg.value("captureUpVk", 0u);
					// Outside native menu focus, character keys need the subclass
					// only while the user is picking a key.
					ReconcileInputWidgetSubclass();
					if (wasCaptured && !captured) {
						// Overlay session over: an accelerator down without its up
						// (session closed mid-press) would leave a stale entry that
						// misclassifies a later real press.
						handledKeys.clear();
					}
				} else if (type == "destroyView") {
					auto* view = ResolveView(a_msg);
					if (!view) return;
					log.Info(std::format("destroying view '{}'", view->id));
					DestroyOneView(*view);
					const bool wasActive = view == active;
					std::erase_if(views, [view](const std::unique_ptr<View>& a_v) {
						return a_v.get() == view;
					});
					if (wasActive) {
						active = views.empty() ? nullptr : views.front().get();
					}
					// The destroyed view may have been the reveal the batch's hides
					// were waiting on.
					if (!AnyRevealPending()) ApplyDeferredHides();
				} else if (type == "shutdown") {
					log.Info(std::format(
						"shutdown requested by the game (accelEvents={}, frames={})",
						accelEvents, frameSerial));
					quit.store(true);
				} else {
					log.Warn("unknown message type '" + type + "' ignored");
				}
			}

			void DrainCommands()
			{
				for (;;) {
					json msg;
					{
						std::scoped_lock lock(commandMutex);
						if (commands.empty()) break;
						msg = std::move(commands.front());
						commands.pop_front();
					}
					HandleCommand(msg);
				}
				// Hides deferred within this batch apply now, unless a reveal is
				// still waiting on its incoming view's first painted frame.
				if (!AnyRevealPending()) ApplyDeferredHides();
			}

			void CloseWebResources()
			{
				focusGranted = false;
				captureArmed = false;
				ReconcileInputWidgetSubclass();
				ApplyMouseCapture();
				captureClosing.store(true);
				if (framePool) {
					try { framePool.FrameArrived(frameToken); } catch (...) {}
				}
				if (captureSession) {
					captureSession.Close();
					captureSession = nullptr;
				}
				captureCadenceHz = 0;
				if (framePool) {
					framePool.Close();
					framePool = nullptr;
				}
				captureItem = nullptr;
				{
					std::scoped_lock lock(ringMutex);
					ReleaseRing();
				}
				for (auto& view : views) {
					DestroyOneView(*view);
				}
				views.clear();
				active = nullptr;
				environment.Reset();
				captureStarted = false;
			}

			int Run()
			{
				winrt::init_apartment(winrt::apartment_type::single_threaded);
				int exitCode = 0;
				if (CreateWindows() && InitializeComposition()) {
					const HANDLE waits[2] = { wakeEvent, gameProcess };
					while (!quit.load()) {
						// Short timeout only while a reveal awaits its paint sentinel,
						// so the timeout fallback stays responsive.
						const DWORD wait = ::MsgWaitForMultipleObjectsEx(
							2, waits, AnyRevealPending() ? 50 : AnyVisibleRenderStats() ? 100 : 1000,
							QS_ALLINPUT, MWMO_INPUTAVAILABLE);
						if (wait == WAIT_OBJECT_0 + 1) {
							log.Info("game process exited — shutting down");
							break;
						}
						if (pipeDead.load()) {
							log.Info("pipe closed — shutting down");
							break;
						}
						DrainCommands();
						TickReveals();
						TickRenderStats();
						MSG message{};
						while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
							::TranslateMessage(&message);
							::DispatchMessageW(&message);
						}
					}
				} else {
					exitCode = 5;
				}

				Send(json{ { "type", "bye" },
					{ "reason", !byeReason.empty() ? byeReason.c_str()
							: exitCode == 0      ? "shutdown"
												 : "init-failed" } });
				log.pipe = nullptr;
				CloseWebResources();
				if (dispatcher) {
					try { dispatcher.ShutdownQueueAsync(); } catch (...) {}
					dispatcher = nullptr;
				}
				rootVisual = nullptr;
				compositor = nullptr;
				captureDevice = nullptr;
				produceFence.Reset();
				consumeFence.Reset();
				context4.Reset();
				context.Reset();
				device5.Reset();
				device.Reset();
				if (hostWindow) {
					::DestroyWindow(hostWindow);
					hostWindow = nullptr;
				}
				hostWindowProc = nullptr;
				s_hostInputApp = nullptr;
				if (bootstrapWindow) {
					::DestroyWindow(bootstrapWindow);
					bootstrapWindow = nullptr;
				}
				pipe.Close();
				if (reader.joinable()) reader.join();
				log.Info(std::format("host exiting (code {})", exitCode));
				return exitCode;
			}
		};
	}

	int RunHost(const HostOptions& a_options)
	{
		App app;
		app.options = a_options;
		app.log.Open(a_options.logFile);

		bool elevated = false;
		{
			HANDLE token = nullptr;
			if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
				TOKEN_ELEVATION elevation{};
				DWORD size = 0;
				elevated = ::GetTokenInformation(token, TokenElevation,
								&elevation, sizeof(elevation), &size) &&
				           elevation.TokenIsElevated;
				::CloseHandle(token);
			}
		}
		wchar_t exePath[MAX_PATH]{};
		::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
		app.log.Info(std::format(
			"osfui_webview2_host starting (pid {}, game pid {}, pipe '{}', elevated={}, exe '{}')",
			::GetCurrentProcessId(), a_options.gamePid, ToUtf8(a_options.pipeName),
			elevated ? "yes" : "no", ToUtf8(exePath)));

		// One host per game process: a relaunch while a previous host is alive must
		// not fight over the pipe/windows.
		const auto mutexName = std::format(L"Local\\osfui-wv2-host-{}", a_options.gamePid);
		const HANDLE instanceMutex = ::CreateMutexW(nullptr, TRUE, mutexName.c_str());
		if (!instanceMutex || ::GetLastError() == ERROR_ALREADY_EXISTS) {
			app.log.Error("another host instance is already running for this game pid");
			return 3;
		}

		// Pipe before OpenProcess: once log.pipe is set, every warning/error below
		// is forwarded into the game's own log, so a startup death is diagnosable
		// from "OSF UI.log" alone. The game tolerates log messages before hello.
		if (!app.pipe.Connect(a_options.pipeName, 15000)) {
			app.log.Error("pipe connect failed: " + app.pipe.LastErrorText());
			::CloseHandle(instanceMutex);
			return 2;
		}
		app.log.pipe = &app.pipe;

		app.gameProcess = ::OpenProcess(
			PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, a_options.gamePid);
		if (!app.gameProcess) {
			const auto error = ::GetLastError();
			auto message = std::format("OpenProcess(game pid {}) failed ({})",
				a_options.gamePid, error);
			if (error == ERROR_ACCESS_DENIED) {
				message += std::format(
					" — access denied: the game is likely running elevated (as "
					"administrator) while this host is not (elevated={}); run the "
					"game/MO2 without administrator rights",
					elevated ? "yes" : "no");
			}
			app.log.Error(message);
			::CloseHandle(instanceMutex);
			return 4;
		}

		LPWSTR runtimeVersion = nullptr;
		std::string runtime = "unknown";
		if (SUCCEEDED(::GetAvailableCoreWebView2BrowserVersionString(nullptr, &runtimeVersion)) &&
			runtimeVersion) {
			runtime = ToUtf8(runtimeVersion);
			::CoTaskMemFree(runtimeVersion);
		} else {
			PromptInstallWebView2Runtime(app.log);
		}
		app.Send(json{
			{ "type", "hello" },
			{ "protocolVersion", kProtocolVersion },
			{ "hostVersion", OSFUI::kPluginVersion },
			{ "runtimeVersion", runtime },
			{ "pid", ::GetCurrentProcessId() },
		});
		app.log.Info("hello sent (WebView2 runtime " + runtime + ")");

		app.wakeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
		app.reader = std::thread([&app] { app.ReaderMain(); });

		const int code = app.Run();
		::CloseHandle(app.gameProcess);
		::CloseHandle(app.wakeEvent);
		::CloseHandle(instanceMutex);
		return code;
	}
}
