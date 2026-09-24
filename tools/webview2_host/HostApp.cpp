#include "HostApp.h"
#include "EmbeddedScripts.h"

#include "Core/Version.h"
#include "Core/Ids.h"
#include "Core/Json.h"
#include "Views/ViewCache.h"
#include "Wv2BoundedQueue.h"
#include "Wv2LocalUri.h"
#include "Wv2Messages.h"
#include "Wv2CdpInput.h"
#include "Wv2MouseButtons.h"
#include "Wv2Pipe.h"
#include "Wv2Protocol.h"
#include "Win32Util.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
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

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using nlohmann::json;

namespace osfui::wv2
{
	namespace
	{
		using osfui::win32::ToUtf8;
		using osfui::win32::ToWide;

		namespace Json = OSFUI::Json;

		// Wire message shapes, compiled by the game side too (Wv2Messages.h).
		namespace msg = osfui::wv2::msg;

		struct Logger
		{
			std::ofstream file;
			std::mutex    mutex;
			std::atomic<Pipe*> pipe{ nullptr };  // set once the pipe is up; nulled at teardown

			void Open(const std::filesystem::path& a_path)
			{
				if (a_path.empty()) return;
				std::error_code ec;
				std::filesystem::create_directories(a_path.parent_path(), ec);
				if (std::filesystem::exists(a_path, ec)) {
					auto old = a_path;
					old.replace_extension(".old.log");
					std::filesystem::rename(a_path, old, ec);
				}
				file.open(a_path, std::ios::out | std::ios::trunc);
			}

			// level: 0 info, 1 warn, 2 error
			void Log(int a_level, const std::string& a_text)
			{
				{
					std::scoped_lock lock(mutex);
					if (file.is_open()) {
						SYSTEMTIME localTime{};
						::GetLocalTime(&localTime);
						file << std::format("[{:02}-{:02} {:02}:{:02}:{:02}.{:03}] [{}] {}\n",
							localTime.wMonth, localTime.wDay, localTime.wHour, localTime.wMinute,
							localTime.wSecond, localTime.wMilliseconds,
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
				if (auto* target = pipe.load(std::memory_order_acquire)) {
					target->WriteMessage(Json::Dump(msg::ToJson(
						msg::Log{ .level = a_level, .text = a_text })));
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

		struct SharedReadLease
		{
			HANDLE handle{ INVALID_HANDLE_VALUE };

			~SharedReadLease()
			{
				if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle);
			}

			bool Open(const std::filesystem::path& a_path)
			{
				if (handle != INVALID_HANDLE_VALUE) return true;
				const HANDLE mutex = ::CreateMutexW(nullptr, FALSE, OSFUI::ViewCache::kMutexName);
				if (!mutex) return false;
				const auto wait = ::WaitForSingleObject(mutex, 30000);
				const bool owned = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
				if (!owned) {
					::CloseHandle(mutex);
					::SetLastError(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_LOCK_FAILED);
					return false;
				}
				handle = ::CreateFileW(a_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_HIDDEN, nullptr);
				const auto error = handle == INVALID_HANDLE_VALUE ? ::GetLastError() : ERROR_SUCCESS;
				::ReleaseMutex(mutex);
				::CloseHandle(mutex);
				if (error != ERROR_SUCCESS) ::SetLastError(error);
				return handle != INVALID_HANDLE_VALUE;
			}
		};

		constexpr wchar_t kRuntimeDownloadUrl[] = L"https://go.microsoft.com/fwlink/p/?LinkId=2124703";

		void PromptInstallWebView2Runtime(Logger& a_log)
		{
			static std::atomic_bool prompted{ false };
			if (prompted.exchange(true)) return;
			a_log.Error("the WebView2 Evergreen Runtime is not installed — showing the install dialog (download: https://go.microsoft.com/fwlink/p/?LinkId=2124703)");
			std::thread([] {
				const auto choice = ::MessageBoxW(nullptr,
					L"OSF UI cannot start because the Microsoft Edge WebView2 Runtime is not installed on this PC.\n\n"
					L"Mod WebViews will not appear without it. OSF Settings remains available.\n\n"
					L"Open the download in your browser now? Run the downloaded \"MicrosoftEdgeWebview2Setup.exe\", then restart the game.",

					L"OSF UI - WebView2 Runtime missing",
					MB_YESNO | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
				if (choice == IDYES) {
					::ShellExecuteW(nullptr, L"open", kRuntimeDownloadUrl, nullptr, nullptr, SW_SHOWNORMAL);
				}
			}).detach();
		}

		void PromptRepairWebView2Runtime(Logger& a_log, HRESULT a_hr)
		{
			static std::atomic_bool prompted{ false };
			if (prompted.exchange(true)) return;
			const auto code = static_cast<unsigned>(a_hr);
			a_log.Error(std::format(
				"WebView2 could not create its composition controller (0x{:08X}) - "
				"showing the repair dialog", code));
			std::thread([code] {
				const auto message = std::format(
					L"OSF UI could not start the Microsoft Edge WebView2 renderer "
					L"(error 0x{:08X}).\n\n"
					L"The overlay has been closed so the game remains usable. Restart "
					L"Windows, then repair or reinstall the WebView2 Runtime. If the "
					L"problem remains, repair the Microsoft Visual C++ x64 Runtime.\n\n"
					L"Open Microsoft's WebView2 installer download now?",
					code);
				const auto choice = ::MessageBoxW(nullptr, message.c_str(),
					L"OSF UI - WebView2 renderer failed",
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
			bool             rendererFatal{ false };  // STA thread only; first failure wins
			std::string      byeReason;  // overrides the default bye reason (STA thread only)
			std::uint64_t    gameWindowMissingSince{ 0 };  // STA thread; 0 while HWND is valid

			static constexpr std::size_t kMaxGameMessages = 1024;
			static constexpr std::size_t kGameMessagesPerDrain = 128;
			BoundedQueue<json> gameMessages{ kMaxGameMessages };
			std::atomic_bool pipeDead{ false };
			std::atomic_bool gameMessageOverflow{ false };
			std::atomic_bool shutdownRequested{ false };
			std::uint64_t nextHeartbeatAt{ 0 };  // STA thread only

			// Init state from the game.
			bool                  initialized{ false };
			HWND                  gameTopLevel{ nullptr };
			std::filesystem::path viewsRoot, userData;
			SharedReadLease       viewsLease;  // released after every WebView member
			std::uint32_t         width{ 1 }, height{ 1 };
			std::uint32_t         viewportWidth{ 1 }, viewportHeight{ 1 };
			bool                  devMode{ false };
			bool                  highRefreshCapture{ false };
			bool                  windowActive{ true };
			bool                  defaultHidden{ true };  // init.hidden — a new view's starting state

			HWND bootstrapWindow{ nullptr };
			HWND hostWindow{ nullptr };

			struct View
			{
				std::string id;
				std::wstring modId;
				std::wstring viewName;
				HWND        window{ nullptr };
				winrt::Windows::UI::Composition::ContainerVisual visual{ nullptr };
				ComPtr<ICoreWebView2Controller>            controller;
				ComPtr<ICoreWebView2CompositionController> compositionController;
				ComPtr<ICoreWebView2>                      webView;
				ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> consoleReceiver;
				ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> exceptionReceiver;
				bool controllerRequested{ false };
				bool securityReady{ false };
				bool hidden{ true };
				bool nonGestureOpenWarned{ false };
				bool navigationBlockedWarned{ false };
				bool frameNavigationBlockedWarned{ false };
				std::uint64_t pageMessageWindowStarted{ 0 };
				std::uint32_t pageMessagesThisWindow{ 0 };
				bool pageMessageTooLargeWarned{ false };
				bool pageMessageFloodWarned{ false };
				std::uint32_t logicalHeight{ kDefaultLogicalHeight };
				bool          revealPending{ false };
				bool          hideDeferred{ false };
				std::uint64_t revealDeadline{ 0 };
				std::string   revealToken;
				std::uint64_t pendingPresentationEpoch{ 0 };
				int  order{ 0 };
				bool domSeen{ false };
				std::wstring currentUrl;
				std::optional<std::wstring> pendingNavigate;
				std::deque<std::string> queuedPostWeb;
				SyntheticMouseButtons syntheticMouseButtons;
				std::shared_ptr<CdpInputQueue> cdpInput;
				CdpPressedKeys cdpKeys;
				std::optional<bool> cdpFocused;
				bool cdpComposition{ false };
			};
			std::vector<std::unique_ptr<View>> views;  // creation order (= z tie-break)
			View* inputTarget{ nullptr };  // mouse/focus/synthetic-key target
			bool  captureStarted{ false };
			bool          focusGranted{ false };
			bool          pointerInputEnabled{ true };
			std::uint64_t focusEpoch{ 0 };
			int           capturedMouseX{ 0 }, capturedMouseY{ 0 };
			std::uint64_t syntheticMouseRecoveryCount{ 0 };
			static constexpr std::size_t kMaxEgressWarnsPerView = 32;
			std::unordered_map<std::string, std::unordered_set<std::string>> egressWarned;
			std::uint64_t cdpKeyEvents{ 0 }, cdpTextEvents{ 0 };

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
			std::atomic_bool   captureHasVisibleView{ false };
			std::uint32_t      captureCadenceHz{ 0 };

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
			ComPtr<ID3D11Fence> produceFence;
			std::uint64_t              frameSerial{ 0 };
			std::uint32_t              lastSlot{ 0 };
			std::uint64_t              republishEpoch{ 0 }; // ringMutex
			std::mutex                 captureEpochMutex;
			std::atomic<std::uint64_t> presentationEpoch{ 0 };
			std::array<std::atomic<std::uint64_t>, kRingSlots> ackedSerials{};
			std::uint64_t consumeLagDrops{ 0 };

			ComPtr<ICoreWebView2Environment> environment;
			bool environmentRequested{ false };

			bool Send(const json& a_msg)
			{
				if (pipe.WriteMessage(Json::Dump(a_msg))) return true;
				pipeDead.store(true, std::memory_order_release);
				if (wakeEvent) ::SetEvent(wakeEvent);
				return false;
			}

			void ReaderMain()
			{
				std::string payload;
				while (pipe.ReadMessage(payload)) {
					auto message = Json::Parse(payload);
					if (!message) {
						log.Warn("dropping unparseable pipe message");
						continue;
					}
					json&      parsed = *message;
					const auto type = Json::Get(parsed, "type", "");
					if (type == "shutdown") {
						shutdownRequested.store(true, std::memory_order_release);
						quit.store(true, std::memory_order_release);
						log.Info("shutdown request received from the game");
						::SetEvent(wakeEvent);
						break;
					}

					const auto coalesceKey = GameMessageCoalesceKey(type,
						Json::Get(parsed, "kind", ""),
						Json::Get(parsed, "view", ""));
					const auto result =
						gameMessages.Push(std::move(parsed), coalesceKey);
					if (result == decltype(gameMessages)::PushResult::Full) {
						gameMessageOverflow.store(true, std::memory_order_release);
						quit.store(true, std::memory_order_release);
						log.Error(std::format(
							"game-message queue exceeded {} messages; closing the browser host",
							kMaxGameMessages));
						::SetEvent(wakeEvent);
						break;
					}
					if (result == decltype(gameMessages)::PushResult::Closed) break;
					::SetEvent(wakeEvent);
				}
				pipeDead.store(true, std::memory_order_release);
				::SetEvent(wakeEvent);
			}
#include "HostGraphics.inl"
#include "CdpInput.inl"

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


			static constexpr std::string_view kRevealSentinelPrefix = "__osfuiRevealReady:";
			static constexpr std::uint64_t kRevealTimeoutMs = 300;

			std::string NewRevealToken()
			{
				GUID guid{};
				wchar_t guidText[40]{};
				if (SUCCEEDED(::CoCreateGuid(&guid)) &&
					::StringFromGUID2(guid, guidText, static_cast<int>(std::size(guidText))) > 0) {
					return std::string(kRevealSentinelPrefix) + ToUtf8(guidText);
				}
				static std::atomic<std::uint64_t> fallback{ 0 };
				return std::format("{}{}-{}",
					kRevealSentinelPrefix,
					::GetCurrentProcessId(),
					fallback.fetch_add(1, std::memory_order_relaxed) + 1);
			}

			bool AnyRevealPending() const
			{
				for (const auto& view : views) {
					if (view->revealPending) return true;
				}
				return false;
			}

			void RefreshCaptureVisibility()
			{
				const bool anyVisible = std::ranges::any_of(views, [](const std::unique_ptr<View>& a_view) { return !a_view->hidden; });
				const bool changed = captureHasVisibleView.exchange(
					anyVisible, std::memory_order_acq_rel) != anyVisible;
				if (changed) ApplyCaptureCadence();
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
				RecoverPressedMouseButtons(a_view, "view hide");
				SetCdpFocus(a_view, false);
				a_view.hidden = true;
				RefreshCaptureVisibility();
				a_view.pendingPresentationEpoch = 0;
				a_view.revealPending = false;  // cancel an in-flight reveal
				a_view.revealToken.clear();
				a_view.hideDeferred = true;    // applied at batch end / reveal end
				log.Info(std::format("view '{}': hide (deferred to batch end)", a_view.id));
			}

			void ShowView(View& a_view)
			{
				if (!a_view.hidden) {
					a_view.hideDeferred = false;
					log.Info(std::format("view '{}': show — already visible (visual={})",
						a_view.id, a_view.visual && a_view.visual.IsVisible()));
					if (PromotePresentation(a_view)) {
						RepublishLatest();
					}
					if (focusGranted && inputTarget == &a_view && a_view.controller) {
						ReconcileCdpFocus();
					}
					return;
				}
				a_view.hidden = false;
				RefreshCaptureVisibility();
				a_view.hideDeferred = false;
				if (a_view.controller) a_view.controller->put_IsVisible(TRUE);
				ReconcileCdpFocus();
				if (a_view.visual && a_view.visual.IsVisible()) {
					log.Info(std::format(
						"view '{}': show — hide was still deferred, never left the screen", a_view.id));
					if (PromotePresentation(a_view)) {
						RepublishLatest();
					}
					return;
				}
				if (a_view.visual && a_view.webView && a_view.domSeen) {
					a_view.revealPending = true;
					a_view.revealToken = NewRevealToken();
					a_view.revealDeadline = ::GetTickCount64() + kRevealTimeoutMs;
					log.Info(std::format("view '{}': show — reveal pending ({} ms timeout)",
						a_view.id, kRevealTimeoutMs));
					const auto revealScript = std::format(
						L"requestAnimationFrame(function(){{requestAnimationFrame(function(){{"
						L"chrome.webview.postMessage('{}');}});}});",
						ToWide(a_view.revealToken));
					a_view.webView->ExecuteScript(revealScript.c_str(),
						Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
							[](HRESULT, LPCWSTR) -> HRESULT { return S_OK; }).Get());
				} else {
					log.Info(std::format(
						"view '{}': show — direct (visual={} webView={} domSeen={})", a_view.id,
						a_view.visual != nullptr, a_view.webView != nullptr, a_view.domSeen));
					if (a_view.visual) a_view.visual.IsVisible(true);
					PromoteChangedPresentation(a_view);
				}
			}

			void CompleteReveal(View& a_view, bool a_timedOut)
			{
				if (!a_view.revealPending) return;
				a_view.revealPending = false;
				a_view.revealToken.clear();
				if (a_timedOut) {
					log.Info(std::format(
						"view '{}': reveal sentinel timed out — showing anyway", a_view.id));
				} else {
					log.Info(std::format("view '{}': reveal sentinel arrived — showing", a_view.id));
				}
				if (a_view.visual && !a_view.hidden) a_view.visual.IsVisible(true);
				if (!AnyRevealPending()) ApplyDeferredHides();
				PromoteChangedPresentation(a_view);
			}

			void TickReveals()
			{
				const auto now = ::GetTickCount64();
				for (auto& view : views) {
					if (view->cdpInput) view->cdpInput->CheckTimeout();
					if (view->revealPending && now >= view->revealDeadline) {
						CompleteReveal(*view, /*a_timedOut=*/true);
					}
				}
			}

			void DestroyOneView(View& a_view)
			{
				SuspendCdpInput(a_view);
				RecoverPressedMouseButtons(a_view, "view destruction");
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
				a_view.exceptionReceiver.Reset();
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
							for (auto& view : views) {
								RequestController(*view);
							}
							return S_OK;
						});
				auto environmentOptions = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
				if (!environmentOptions) {
					log.Error("could not allocate WebView2 environment options");
					environmentRequested = false;
					return false;
				}
				constexpr wchar_t kCaptureBrowserArguments[] =
					L"--disable-backgrounding-occluded-windows --disable-renderer-backgrounding "
					L"--disable-features=CalculateNativeWinOcclusion,ApplyNativeOcclusionToCompositor,"
					L"WindowsScrollingPersonality,PercentBasedScrolling";
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
					"WebView2 renderer and native-occlusion throttling disabled for the offscreen capture browser host");
				const auto hr = ::CreateCoreWebView2EnvironmentWithOptions(
					nullptr, userData.c_str(), environmentOptions.Get(), callback.Get());
				if (FAILED(hr)) {
					log.Error(std::format("CreateCoreWebView2EnvironmentWithOptions failed (0x{:08X})",
						static_cast<unsigned>(hr)));
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

			void ReportControllerFailure(View& a_view, HRESULT a_hr,
				std::string_view a_description)
			{
				if (rendererFatal) return;
				rendererFatal = true;
				const auto code = static_cast<unsigned>(a_hr);
				log.Error(std::format("view '{}': {} (0x{:08X})",
					a_view.id, a_description, code));
				Send(msg::ToJson(msg::Fatal{
					.stage = "composition-controller",
					.view = a_view.id,
					.description = std::string(a_description),
					.code = code,
				}));
				PromptRepairWebView2Runtime(log, a_hr);
			}

			void ReportSecurityFailure(View& a_view, HRESULT a_hr,
				std::string_view a_description)
			{
				if (rendererFatal) return;
				rendererFatal = true;
				byeReason = "security-policy-failed";
				const auto code = static_cast<unsigned>(a_hr);
				log.Error(std::format("view '{}': {} (0x{:08X}); refusing to run "
									 "without the complete browser security policy",
					a_view.id, a_description, code));
				Send(msg::ToJson(msg::Fatal{
					.stage = "browser-security",
					.view = a_view.id,
					.description = std::string(a_description),
					.code = code,
				}));
				if (a_view.webView) a_view.webView->Stop();
				quit.store(true);
				if (wakeEvent) ::SetEvent(wakeEvent);
			}

			HRESULT HandleNavigationStarting(View& a_view, ICoreWebView2NavigationStartingEventArgs* a_args, bool a_frame)
			{
				LPWSTR raw = nullptr;
				const auto uriHr = a_args ? a_args->get_Uri(&raw) : E_POINTER;
				if (FAILED(uriHr) || !raw) {
					if (a_args) a_args->put_Cancel(TRUE);
					ReportSecurityFailure(a_view, FAILED(uriHr) ? uriHr : E_POINTER, a_frame ? "frame navigation source was unavailable" : "top-level navigation source was unavailable");
					return S_OK;
				}
				std::wstring uri(raw);
				::CoTaskMemFree(raw);
				const bool allowed = IsTrustedViewDocumentUri(uri, kViewHost, a_view.modId, a_view.viewName) || (a_frame && IsAllowedBlankFrameUri(uri));
				if (allowed) {
					if (!a_frame) {
						RecoverPressedMouseButtons(a_view, "main-frame navigation");
						a_view.domSeen = false;
						SuspendCdpInput(a_view);
					}
					return S_OK;
				}

				const auto cancelHr = a_args->put_Cancel(TRUE);
				if (FAILED(cancelHr)) {
					ReportSecurityFailure(a_view, cancelHr, a_frame ? "could not cancel an untrusted frame navigation" : "could not cancel an untrusted top-level navigation");
					return S_OK;
				}
				auto& warned = a_frame ? a_view.frameNavigationBlockedWarned : a_view.navigationBlockedWarned;
				if (!warned) {
					warned = true;
					log.Warn(std::format("view '{}': blocked {} navigation to {} (further attempts are silent)", a_view.id, a_frame ? "frame" : "top-level", ToUtf8(uri)));
				}
				return S_OK;
			}

			HRESULT InstallOriginGuards(View& a_view)
			{
				View* view = &a_view;
				EventRegistrationToken token{};
				auto hr = a_view.webView->add_NavigationStarting(
					Callback<ICoreWebView2NavigationStartingEventHandler>(
						[this, view](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a_args) -> HRESULT {
							return HandleNavigationStarting(*view, a_args, false);
						}).Get(), &token);
				if (FAILED(hr)) return hr;
				hr = a_view.webView->add_FrameNavigationStarting(
					Callback<ICoreWebView2NavigationStartingEventHandler>(
						[this, view](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a_args) -> HRESULT {
							return HandleNavigationStarting(*view, a_args, true);
						}).Get(), &token);
				return hr;
			}

			HRESULT OnController(View& a_view, HRESULT a_hr,
				ICoreWebView2CompositionController* a_composition)
			{
				if (quit.load()) return S_OK;
				if (FAILED(a_hr) || !a_composition) {
					const auto failureHr = FAILED(a_hr) ? a_hr : E_POINTER;
					ReportControllerFailure(a_view, failureHr,
						failureHr == static_cast<HRESULT>(0x800736B1u) ?
						"composition controller failed: Windows side-by-side activation is broken" :
						"composition controller callback failed");
					return S_OK;
				}
				a_view.compositionController = a_composition;

				if (FAILED(a_view.compositionController.As(&a_view.controller)) ||
					FAILED(a_view.controller->get_CoreWebView2(&a_view.webView)) || !a_view.webView) {
					ReportControllerFailure(a_view, E_NOINTERFACE,
						"composition controller created but CoreWebView2 was unavailable");
					return S_OK;
				}
				ComPtr<ICoreWebView2Settings> settings;
				if (SUCCEEDED(a_view.webView->get_Settings(&settings)) && settings) {
					settings->put_AreDefaultContextMenusEnabled(FALSE);
					settings->put_AreDevToolsEnabled(devMode ? TRUE : FALSE);
				} else {
					log.Warn(std::format("view '{}': settings unavailable — the browser "
						"context menu stays enabled", a_view.id));
				}
				a_view.controller->put_Bounds(
					RECT{ 0, 0, static_cast<LONG>(viewportWidth), static_cast<LONG>(viewportHeight) });
				ApplyScale(a_view);
				a_view.controller->put_IsVisible(a_view.hidden ? FALSE : TRUE);
				ComPtr<ICoreWebView2Controller2> controller2;
				if (SUCCEEDED(a_view.controller.As(&controller2))) {
					controller2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{ 0, 0, 0, 0 });
				}
				a_view.visual = compositor.CreateContainerVisual();
				a_view.visual.Size({ static_cast<float>(viewportWidth), static_cast<float>(viewportHeight) });
				a_view.visual.Offset({
					(static_cast<float>(width) - static_cast<float>(viewportWidth)) * 0.5f,
					(static_cast<float>(height) - static_cast<float>(viewportHeight)) * 0.5f,
					0.0f });
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
					ReportSecurityFailure(a_view, E_NOINTERFACE, "virtual-host mapping API unavailable");
					return S_OK;
				}
				result = webView3->SetVirtualHostNameToFolderMapping(kViewHost.data(), viewsRoot.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
				if (FAILED(result)) {
					ReportSecurityFailure(a_view, result, "OSF UI virtual-host mapping failed");
					return S_OK;
				}
				result = InstallOriginGuards(a_view);
				if (FAILED(result)) {
					ReportSecurityFailure(a_view, result, "navigation origin-policy installation failed");
					return S_OK;
				}
				result = InstallNetworkGuard(a_view);
				if (FAILED(result)) {
					ReportSecurityFailure(a_view, result, "network egress policy installation failed");
				}
				return S_OK;
			}

			HRESULT AddDocumentScript(View& a_view, const EmbeddedScript a_script, std::function<void(HRESULT)> a_completion)
			{
				const auto& source = GetEmbeddedScript(a_script);
				if (!a_view.webView) return E_POINTER;
				if (source.empty()) return HRESULT_FROM_WIN32(ERROR_RESOURCE_DATA_NOT_FOUND);
				return a_view.webView->AddScriptToExecuteOnDocumentCreated(source.c_str(),
					Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
						[completion = std::move(a_completion)](HRESULT a_hr, LPCWSTR) -> HRESULT {
							completion(a_hr);
							return S_OK;
						}).Get());
			}
			void FinishControllerSetup(View& a_view)
			{
				if (quit.load() || a_view.securityReady || !a_view.webView) return;
				a_view.securityReady = true;
				InstallEvents(a_view);
				InstallBridgeShim(a_view);
				if (!captureStarted) {
					if (!StartCapture()) return;
					captureStarted = true;
					Send(msg::ToJson(msg::Ready{}));
				}
				log.InfoFwd(std::format("view '{}': controller ready ({} view(s) hosted)",
					a_view.id, views.size()));
				DrainQueuedViewWork(a_view);
				ReconcileCdpFocus();
			}

			void InstallBridgeShim(View& a_view)
			{
				const auto hr = AddDocumentScript(a_view, EmbeddedScript::BridgeShim,
					[this](const HRESULT a_scriptHr) {
						if (FAILED(a_scriptHr)) {
							log.Error(std::format("bridge shim install failed (0x{:08X})",
								static_cast<unsigned>(a_scriptHr)));
						}
					});
				if (FAILED(hr)) {
					log.Error(std::format("bridge shim registration failed (0x{:08X})",
						static_cast<unsigned>(hr)));
				}
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

			HRESULT InstallNetworkGuard(View& a_view)
			{
				View* view = &a_view;
				ComPtr<ICoreWebView2_22> webView22;
				auto filterHr = a_view.webView.As(&webView22);
				if (FAILED(filterHr) || !webView22) {
					log.Error(std::format(
						"view '{}': source-kind egress filter is unavailable (0x{:08X})",
						a_view.id, static_cast<unsigned>(filterHr)));
					return FAILED(filterHr) ? filterHr : E_NOINTERFACE;
				}
				for (const auto* pattern : { L"http://*", L"https://*" }) {
					filterHr = webView22->AddWebResourceRequestedFilterWithRequestSourceKinds(
						pattern, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL,
						COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL);
					if (FAILED(filterHr)) {
						log.Error(std::format(
							"view '{}': source-kind egress filter registration failed (0x{:08X})",
							a_view.id, static_cast<unsigned>(filterHr)));
						return filterHr;
					}
				}
				EventRegistrationToken token{};
				const auto eventHr = a_view.webView->add_WebResourceRequested(
					Callback<ICoreWebView2WebResourceRequestedEventHandler>(
						[this, view](ICoreWebView2*,
							ICoreWebView2WebResourceRequestedEventArgs* a_args) -> HRESULT {
							if (!a_args) {
								ReportSecurityFailure(*view, E_POINTER, "intercepted web-request arguments were unavailable");
								return S_OK;
							}
							ComPtr<ICoreWebView2WebResourceRequest> request;
							const auto requestHr = a_args->get_Request(&request);
							if (FAILED(requestHr) || !request) {
								ReportSecurityFailure(*view, FAILED(requestHr) ? requestHr : E_POINTER, "could not inspect an intercepted web request");
								return S_OK;
							}
							LPWSTR raw = nullptr;
							const auto uriHr = request->get_Uri(&raw);
							if (FAILED(uriHr) || !raw) {
								ReportSecurityFailure(*view, FAILED(uriHr) ? uriHr : E_POINTER, "could not inspect an intercepted request URI");
								return S_OK;
							}
							std::wstring uri(raw);
							::CoTaskMemFree(raw);
							if (IsAllowedViewResourceUri(uri, kViewHost)) return S_OK;
							ComPtr<ICoreWebView2WebResourceResponse> response;
							const auto responseHr = environment ? environment->CreateWebResourceResponse(nullptr, 403, L"Forbidden", L"", &response) : E_POINTER;
							const auto putHr = SUCCEEDED(responseHr) && response ? a_args->put_Response(response.Get()) : E_POINTER;
							if (FAILED(responseHr) || !response || FAILED(putHr)) {
								ReportSecurityFailure(*view, FAILED(responseHr) ? responseHr : FAILED(putHr) ? putHr : E_POINTER, "could not synthesize the blocked-request response");
								return S_OK;
							}
							auto& warned = egressWarned[view->id];
							if (warned.size() < kMaxEgressWarnsPerView &&
								warned.insert(ToUtf8(UriHost(uri))).second) {
								log.Warn(std::format("view '{}': blocked network egress to {} (further denials for this origin are silent)", view->id, ToUtf8(uri)));
								if (warned.size() == kMaxEgressWarnsPerView) {
									log.Warn(std::format("view '{}': {} distinct blocked origins — further " "egress denials for this view are silent", view->id, kMaxEgressWarnsPerView));
								}
							}
							return S_OK;
						}).Get(), &token);
				if (FAILED(eventHr)) {
					log.Error(std::format(
						"view '{}': WebResourceRequested handler registration failed (0x{:08X})",
						a_view.id, static_cast<unsigned>(eventHr)));
					return eventHr;
				}
				const auto viewId = a_view.id;
				const auto scriptHr = AddDocumentScript(a_view, EmbeddedScript::NetworkGuard,
					[this, viewId](const HRESULT a_scriptHr) {
						auto* current = FindView(viewId);
						if (!current) return;
						if (FAILED(a_scriptHr)) {
							log.Error(std::format("egress neuter script install failed (0x{:08X})",
								static_cast<unsigned>(a_scriptHr)));
							ReportSecurityFailure(*current, a_scriptHr,
								"egress transport policy installation failed");
						} else {
							FinishControllerSetup(*current);
						}
					});
				if (FAILED(scriptHr)) {
					log.Error(std::format(
						"view '{}': egress neuter script registration failed (0x{:08X})",
						a_view.id, static_cast<unsigned>(scriptHr)));
					return scriptHr;
				}
				return S_OK;
			}

			[[nodiscard]] bool GameIsForeground() const
			{
				return gameTopLevel && !::IsIconic(gameTopLevel) && ::GetForegroundWindow() == gameTopLevel;
			}

			void QueueGameFocusRestore()
			{
				if (gameTopLevel) ::PostMessageW(gameTopLevel, kRestoreGameFocusMessage, 0, 0);
			}

			void InstallEvents(View& a_view)
			{
				View* view = &a_view;
				EventRegistrationToken token{};
				a_view.compositionController->add_CursorChanged(
					Callback<ICoreWebView2CursorChangedEventHandler>(
						[this, view](ICoreWebView2CompositionController* a_sender, ::IUnknown*) -> HRESULT {
							// Only the input-target view drives the real OS pointer.
							if (view != inputTarget) return S_OK;
							UINT32 id = 0;
							if (SUCCEEDED(a_sender->get_SystemCursorId(&id))) {
								Send(msg::ToJson(msg::Cursor{ .id = id }));
							}
							return S_OK;
						}).Get(), &token);
				a_view.controller->add_GotFocus(
					Callback<ICoreWebView2FocusChangedEventHandler>(
						[this](ICoreWebView2Controller*, ::IUnknown*) -> HRESULT {
							// Visibility changes can request browser HWND focus unexpectedly.
							if (GameIsForeground()) QueueGameFocusRestore();
							return S_OK;
						}).Get(), &token);
				a_view.webView->add_WebMessageReceived(
					Callback<ICoreWebView2WebMessageReceivedEventHandler>(
						[this, view](ICoreWebView2*,
							ICoreWebView2WebMessageReceivedEventArgs* a_args) -> HRESULT {
							constexpr std::size_t kMaxPageMessageChars = 64 * 1024;
							constexpr std::size_t kMaxPageMessageBytes = 64 * 1024;
							constexpr std::uint32_t kMaxPageMessagesPerSecond = 128;
							if (!a_args) {
								ReportSecurityFailure(*view, E_POINTER, "web-message arguments were unavailable");
								return S_OK;
							}
							LPWSTR sourceRaw = nullptr;
							const auto sourceHr = a_args->get_Source(&sourceRaw);
							if (FAILED(sourceHr) || !sourceRaw) {
								ReportSecurityFailure(*view, FAILED(sourceHr) ? sourceHr : E_POINTER, "web-message source was unavailable");
								return S_OK;
							}
							std::wstring source(sourceRaw);
							::CoTaskMemFree(sourceRaw);
							if (!IsTrustedViewDocumentUri(
									source, kViewHost, view->modId, view->viewName)) {
								log.Error(std::format("view '{}': rejected native-bound message from {}", view->id, ToUtf8(source)));
								ReportSecurityFailure(*view, E_ACCESSDENIED, "web-message source violated the view origin policy");
								return S_OK;
							}
							LPWSTR value = nullptr;
							if (FAILED(a_args->TryGetWebMessageAsString(&value)) || !value)
								return S_OK;
							std::size_t chars = 0;
							while (chars <= kMaxPageMessageChars && value[chars] != L'\0') ++chars;
							if (chars > kMaxPageMessageChars) {
								::CoTaskMemFree(value);
								if (!view->pageMessageTooLargeWarned) {
									view->pageMessageTooLargeWarned = true;
									log.Warn(std::format(
										"view '{}': dropped page message over the 64 KiB limit",
										view->id));
								}
								return S_OK;
							}
							auto text = ToUtf8(value);
							::CoTaskMemFree(value);
							if (text.size() > kMaxPageMessageBytes) {
								if (!view->pageMessageTooLargeWarned) {
									view->pageMessageTooLargeWarned = true;
									log.Warn(std::format(
										"view '{}': dropped page message over the 64 KiB limit",
										view->id));
								}
								return S_OK;
							}
							const auto now = ::GetTickCount64();
							if (view->pageMessageWindowStarted == 0 ||
								now - view->pageMessageWindowStarted >= 1000) {
								view->pageMessageWindowStarted = now;
								view->pageMessagesThisWindow = 0;
							}
							if (view->pageMessagesThisWindow >= kMaxPageMessagesPerSecond) {
								if (!view->pageMessageFloodWarned) {
									view->pageMessageFloodWarned = true;
									log.Warn(std::format(
										"view '{}': page-message rate exceeded 128/s; excess messages are dropped",
										view->id));
								}
								return S_OK;
							}
							++view->pageMessagesThisWindow;
							if (text.starts_with(kRevealSentinelPrefix)) {
								// Browser-host-internal paint handshake; not forwarded.
								if (view->revealPending && text == view->revealToken) {
									CompleteReveal(*view, /*a_timedOut=*/false);
								}
								return S_OK;
							}
							Send(msg::ToJson(msg::WebMessage{ .view = view->id,
								.json = std::move(text) }));
							return S_OK;
						}).Get(), &token);
				a_view.webView->add_NewWindowRequested(
					Callback<ICoreWebView2NewWindowRequestedEventHandler>(
						[this, view](ICoreWebView2*,
							ICoreWebView2NewWindowRequestedEventArgs* a_args) -> HRESULT {
							a_args->put_Handled(TRUE);
							BOOL userInitiated = FALSE;
							if (FAILED(a_args->get_IsUserInitiated(&userInitiated)) ||
								!userInitiated) {
								if (!view->nonGestureOpenWarned) {
									view->nonGestureOpenWarned = true;
									log.Warn(std::format(
										"view '{}': blocked scripted window.open (no user "
										"gesture); further attempts for this view are silent",
										view->id));
								}
								return S_OK;
							}
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
							Send(msg::ToJson(msg::LoadEvent{
								.view = view->id,
								.failed = success != TRUE,
								.url = ToUtf8(view->currentUrl),
								.description = success ? "" : "WebView2 navigation failed",
								.code = static_cast<std::int32_t>(status) }));
							return S_OK;
						}).Get(), &token);
				ComPtr<ICoreWebView2_2> webView2;
				if (SUCCEEDED(a_view.webView.As(&webView2))) {
					webView2->add_DOMContentLoaded(
						Callback<ICoreWebView2DOMContentLoadedEventHandler>(
							[this, view](ICoreWebView2*, ICoreWebView2DOMContentLoadedEventArgs*) -> HRESULT {
								view->domSeen = true;
								InitializeCdpInput(*view);
								DrainQueuedViewWork(*view);
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
								byeReason = "browser-process-exited";
								quit.store(true);
								break;
							case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED:
							case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE:
								view->domSeen = false;
								SuspendCdpInput(*view);
								Send(msg::ToJson(msg::LoadEvent{
									.view = view->id,
									.failed = true,
									.url = ToUtf8(view->currentUrl),
									.description =
										kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED
											? "WebView2 render process exited"
											: "WebView2 render process unresponsive",
									.code = static_cast<std::int32_t>(kind) }));
								break;
							default:
								// Frame/GPU/utility children restart on their own.
								break;
							}
							return S_OK;
						}).Get(), &token);
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
									Send(msg::ToJson(msg::Console{ .view = view->id,
										.json = ToUtf8(value) }));
									::CoTaskMemFree(value);
								}
								return S_OK;
							}).Get(), &token);
					if (SUCCEEDED(a_view.webView->GetDevToolsProtocolEventReceiver(
							L"Runtime.exceptionThrown", &a_view.exceptionReceiver)) &&
						a_view.exceptionReceiver) {
						a_view.exceptionReceiver->add_DevToolsProtocolEventReceived(
							Callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
								[this, view](ICoreWebView2*,
									ICoreWebView2DevToolsProtocolEventReceivedEventArgs* a_args) -> HRESULT {
									LPWSTR value = nullptr;
									if (FAILED(a_args->get_ParameterObjectAsJson(&value)) || !value) {
										return S_OK;
									}
									const auto raw = ToUtf8(value);
									::CoTaskMemFree(value);
									std::string text = raw;
									if (const auto parsed = Json::Parse(raw)) {
										const json  empty = json::object();
										const auto* found = Json::GetObject(*parsed, "exceptionDetails");
										const json& details = found ? *found : empty;
										const auto* thrown = Json::GetObject(details, "exception");
										text = Json::Get(thrown ? *thrown : empty, "description",
											Json::Get(details, "text", "uncaught exception"));
										const auto url = Json::Get(details, "url", "");
										if (!url.empty()) {
											text += std::format(" ({}:{}:{})", url,
												Json::Get(details, "lineNumber", 0) + 1,
												Json::Get(details, "columnNumber", 0) + 1);
										}
									}
									Send(msg::ToJson(msg::Console{ .view = view->id,
										.json = Json::Dump(json{ { "type", "error" },
											{ "args", json::array({ json{ { "value",
												"uncaught: " + text } } }) } }) }));
									return S_OK;
								}).Get(), &token);
					}
					a_view.webView->CallDevToolsProtocolMethod(L"Runtime.enable", L"{}",
						Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
							[](HRESULT, LPCWSTR) -> HRESULT { return S_OK; }).Get());
				}
			}

			void ApplyCaptureCadence()
			{
				if (!captureSession) return;
				const bool visible = captureHasVisibleView.load(std::memory_order_acquire);
				const std::uint32_t desiredHz = !visible ? 4u :
					highRefreshCapture && focusGranted ? 240u : 60u;
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
							!visible ? "hidden" : focusGranted ? "input-capturing menu" : "HUD",
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
					ApplyCaptureCadence();
					captureSession.StartCapture();
					return true;
				} catch (const winrt::hresult_error& a_error) {
					log.Error(std::format("capture setup failed: {} (0x{:08X})",
						ToUtf8(a_error.message()), static_cast<unsigned>(a_error.code())));
					return false;
				}
			}

			void OnFrameArrived(const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& a_pool)
			{
				if (quit.load() || captureClosing.load()) return;
				try {
					decltype(a_pool.TryGetNextFrame()) capturedFrame{ nullptr };
					std::uint64_t framePresentationEpoch = 0;
					{
						std::scoped_lock epochLock(captureEpochMutex);
						framePresentationEpoch = presentationEpoch.load(std::memory_order_acquire);
						capturedFrame = a_pool.TryGetNextFrame();
					}
					if (!capturedFrame) return;
					// Draining keeps the WGC pool current while closed; no surface access, GPU copy, fence signal, serialization, or pipe write is useful without a visible view.
					if (!captureHasVisibleView.load(std::memory_order_acquire)) return;
					auto access = capturedFrame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
					ComPtr<ID3D11Texture2D> source;
					winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&source)));
					D3D11_TEXTURE2D_DESC desc{};
					source->GetDesc(&desc);
					PublishFrame(source.Get(), desc.Width, desc.Height, framePresentationEpoch);
				} catch (const winrt::hresult_error& a_error) {
					log.Warn(std::format("capture callback failed: {}", ToUtf8(a_error.message())));
				}
			}

			// Game-message handling; STA thread.

			void DrainQueuedViewWork(View& a_view)
			{
				// The controller arrives before the async network guard completes and FinishControllerSetup installs the event handlers and bridge shim.
				if (!a_view.webView || !a_view.securityReady) return;
				if (a_view.pendingNavigate) {
					a_view.domSeen = false;
					SuspendCdpInput(a_view);
					a_view.currentUrl = *a_view.pendingNavigate;
					a_view.pendingNavigate.reset();
					const auto hr = a_view.webView->Navigate(a_view.currentUrl.c_str());
					if (FAILED(hr)) {
						Send(msg::ToJson(msg::LoadEvent{ .view = a_view.id,
							.failed = true,
							.url = ToUtf8(a_view.currentUrl),
							.description = "Navigate returned failure",
							.code = static_cast<std::int32_t>(hr) }));
					}
				}
				if (!a_view.domSeen) return;
				for (auto& message : a_view.queuedPostWeb) {
					const auto wide = ToWide(message);
					a_view.webView->PostWebMessageAsString(wide.c_str());
				}
				a_view.queuedPostWeb.clear();
			}

			void ApplyScale(View& a_view)
			{
				if (!a_view.controller) return;
				ComPtr<ICoreWebView2Controller4> controller4;
				if (FAILED(a_view.controller.As(&controller4)) || !controller4) {
					log.Warn(std::format("view '{}': ICoreWebView2Controller4 unavailable — "
						"rasterization scale not applied (WebView2 Runtime too old)", a_view.id));
					return;
				}
				controller4->put_ShouldDetectMonitorScaleChanges(FALSE);
				const auto logical = (std::max)(1u, a_view.logicalHeight);
				controller4->put_RasterizationScale(
					static_cast<double>(viewportHeight) / static_cast<double>(logical));
			}

			void ApplyViewLayout()
			{
				const float offsetX =
					(static_cast<float>(width) - static_cast<float>(viewportWidth)) * 0.5f;
				const float offsetY =
					(static_cast<float>(height) - static_cast<float>(viewportHeight)) * 0.5f;
				for (auto& view : views) {
					if (view->visual) {
						view->visual.Size({ static_cast<float>(viewportWidth),
							static_cast<float>(viewportHeight) });
						view->visual.Offset({ offsetX, offsetY, 0.0f });
					}
					if (view->controller) {
						view->controller->put_Bounds(RECT{ 0, 0,
							static_cast<LONG>(viewportWidth),
							static_cast<LONG>(viewportHeight) });
						ApplyScale(*view);
					}
				}
			}

			void ApplyViewport(const std::uint32_t a_width,
				const std::uint32_t a_height,
				const std::uint64_t a_presentationEpoch)
			{
				RecoverAllPressedMouseButtons("viewport change");
				viewportWidth = std::clamp((std::max)(1u, a_width), 1u, width);
				viewportHeight = std::clamp((std::max)(1u, a_height), 1u, height);
				ApplyViewLayout();
				{
					std::scoped_lock epochLock(captureEpochMutex);
					presentationEpoch.store(a_presentationEpoch, std::memory_order_release);
				}
				log.Info(std::format(
					"content viewport -> {}x{} inside stable {}x{} capture",
					viewportWidth, viewportHeight, width, height));
			}

			void ApplyResize(std::uint32_t a_width, std::uint32_t a_height)
			{
				RecoverAllPressedMouseButtons("viewport resize");
				width = (std::max)(1u, a_width);
				height = (std::max)(1u, a_height);
				if (rootVisual) {
					rootVisual.Size({ static_cast<float>(width), static_cast<float>(height) });
				}
				viewportWidth = (std::min)(viewportWidth, width);
				viewportHeight = (std::min)(viewportHeight, height);
				ApplyViewLayout();
				if (!framePool) return;
				std::scoped_lock epochLock(captureEpochMutex);
				try {
					framePool.Recreate(captureDevice,
						winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
						3, winrt::Windows::Graphics::SizeInt32{
							static_cast<std::int32_t>(width), static_cast<std::int32_t>(height) });
				} catch (const winrt::hresult_error& a_error) {
					log.Warn(std::format("frame pool resize failed: {}", ToUtf8(a_error.message())));
				}
			}

			[[nodiscard]] static COREWEBVIEW2_MOUSE_EVENT_KIND MouseButtonUpKind(SyntheticMouseButton a_button) noexcept
			{
				switch (a_button) {
				case SyntheticMouseButton::left:
					return COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
				case SyntheticMouseButton::right:
					return COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP;
				case SyntheticMouseButton::middle:
					return COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP;
				}
				return COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
			}

			[[nodiscard]] static COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS MouseVirtualKeys(SyntheticMouseButtonMask a_buttons) noexcept
			{
				UINT32 keys = COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE;
				if ((a_buttons & MouseButtonBit(SyntheticMouseButton::left)) != 0) {
					keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON;
				}
				if ((a_buttons & MouseButtonBit(SyntheticMouseButton::right)) != 0) {
					keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_RIGHT_BUTTON;
				}
				if ((a_buttons & MouseButtonBit(SyntheticMouseButton::middle)) != 0) {
					keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_MIDDLE_BUTTON;
				}
				return static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(keys);
			}

			[[nodiscard]] static std::string DescribeMouseButtons(SyntheticMouseButtonMask a_buttons)
			{
				std::string result;
				for (const auto button : kSyntheticMouseReleaseOrder) {
					if ((a_buttons & MouseButtonBit(button)) == 0) continue;
					if (!result.empty()) result += ',';
					result += MouseButtonName(button);
				}
				return result.empty() ? "none" : result;
			}

			SyntheticMouseButtonMask RecoverPressedMouseButtons(View& a_view, std::string_view a_reason)
			{
				const auto pressed = a_view.syntheticMouseButtons.TakePressed();
				if (pressed == 0) return 0;

				auto remaining = pressed;
				SyntheticMouseButtonMask failed = 0;
				for (const auto button : kSyntheticMouseReleaseOrder) {
					const auto bit = MouseButtonBit(button);
					if ((pressed & bit) == 0) continue;
					remaining &= static_cast<SyntheticMouseButtonMask>(~bit);
					const auto hr = a_view.compositionController ?
						a_view.compositionController->SendMouseInput(
							MouseButtonUpKind(button), MouseVirtualKeys(remaining), 0,
							POINT{ capturedMouseX, capturedMouseY }) :
						E_POINTER;
					if (FAILED(hr)) failed |= bit;
				}

				const auto recovery = ++syntheticMouseRecoveryCount;
				log.Warn(std::format("MouseInputRecovery #{}: view '{}' crossed '{}' with unmatched {} synthetic button-down; forced button-up delivery {}{}", 
					recovery, a_view.id, a_reason, DescribeMouseButtons(pressed), failed == 0 ? "succeeded" : "failed for ", failed == 0 ? "" : DescribeMouseButtons(failed)));
				return pressed;
			}

			void RecoverAllPressedMouseButtons(std::string_view a_reason)
			{
				for (auto& view : views) RecoverPressedMouseButtons(*view, a_reason);
			}

			void SendMouse(const json& a_msg)
			{
				if (!focusGranted || !windowActive || !GameIsForeground()) return;
				if (!pointerInputEnabled || !inputTarget || inputTarget->hidden ||
					!inputTarget->domSeen || !inputTarget->compositionController) return;
				const auto mouse = msg::FromJson<msg::Mouse>(a_msg);
				const std::string& kind = mouse.kind;
				const bool physicalWheel = kind == "physicalWheel";
				int x = mouse.x;
				int y = mouse.y;
				if (kind == "move") {
					capturedMouseX = std::clamp(x, 0, static_cast<int>(viewportWidth) - 1);
					capturedMouseY = std::clamp(y, 0, static_cast<int>(viewportHeight) - 1);
				}
				COREWEBVIEW2_MOUSE_EVENT_KIND eventKind{};
				UINT32 data = 0;
				std::optional<SyntheticMouseButton> trackedButton;
				bool buttonDown = false;
				if (kind == "move") {
					eventKind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE;
				} else if (kind == "wheel" || physicalWheel) {
					eventKind = COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL;
					data = static_cast<UINT32>(mouse.wheel);
				} else {
					const int  button = mouse.button;
					const bool down = mouse.down;
					buttonDown = down;
					if (button == 0) {
						trackedButton = SyntheticMouseButton::left;
						eventKind = down ? COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN : COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
					} else if (button == 1) {
						trackedButton = SyntheticMouseButton::right;
						eventKind = down ? COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN : COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP;
					} else {
						trackedButton = SyntheticMouseButton::middle;
						eventKind = down ? COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN : COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP;
					}
				}
				auto buttons = inputTarget->syntheticMouseButtons;
				if (trackedButton) buttons.Observe(*trackedButton, buttonDown);
				const auto virtualKeys = static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(
					static_cast<UINT32>(MouseVirtualKeys(buttons.Pressed())) | (mouse.modifiers & (MK_SHIFT | MK_CONTROL)));
				const auto hr = inputTarget->compositionController->SendMouseInput(eventKind,
					virtualKeys, data, POINT{ x, y });
				if (SUCCEEDED(hr) && trackedButton) {
					inputTarget->syntheticMouseButtons.Observe(*trackedButton, buttonDown);
				}
			}

#include "GameMessages.inl"

			void DrainGameMessages()
			{
				decltype(gameMessages)::Item item;
				std::size_t drained = 0;
				while (drained < kGameMessagesPerDrain && gameMessages.TryPop(item)) {
					HandleGameMessage(item.value);
					++drained;
				}
				if (gameMessages.Size() != 0) ::SetEvent(wakeEvent);
				if (!AnyRevealPending()) ApplyDeferredHides();
			}

			void CloseWebResources()
			{
				focusGranted = false;
				focusEpoch = 0;
				captureClosing.store(true);
				if (framePool) {
					try { framePool.FrameArrived(frameToken); } catch (...) {}
				}
				if (captureSession) {
					try { captureSession.Close(); }
					catch (const winrt::hresult_error& e) {
						log.Warn(std::format("capture-session close failed (hr=0x{:08X})",
							static_cast<std::uint32_t>(e.code().value)));
					}
					captureSession = nullptr;
				}
				captureCadenceHz = 0;
				if (framePool) {
					std::scoped_lock epochLock(captureEpochMutex);
					try { framePool.Close(); }
					catch (const winrt::hresult_error& e) {
						log.Warn(std::format("capture-pool close failed (hr=0x{:08X})",
							static_cast<std::uint32_t>(e.code().value)));
					}
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
				inputTarget = nullptr;
				environment.Reset();
				captureStarted = false;
			}

			bool SendHeartbeatIfDue()
			{
				const auto now = ::GetTickCount64();
				if (nextHeartbeatAt != 0 && now < nextHeartbeatAt) return true;
				if (!Send(msg::ToJson(msg::Heartbeat{ .tick = now }))) {
					quit.store(true, std::memory_order_release);
					return false;
				}
				nextHeartbeatAt = now + kHeartbeatIntervalMs;
				return true;
			}
			int Run()
			{
				winrt::init_apartment(winrt::apartment_type::single_threaded);
				int exitCode = 0;
				if (CreateWindows() && InitializeComposition()) {
					const HANDLE waits[2] = { wakeEvent, gameProcess };
					const auto captureGameExit = [this](DWORD a_waitMs) {
						if (::WaitForSingleObject(gameProcess, a_waitMs) != WAIT_OBJECT_0) {
							return false;
						}
						DWORD code = 0;
						if (!::GetExitCodeProcess(gameProcess, &code) || code == STILL_ACTIVE) {
							return false;
						}
						log.Info(std::format(
							"game process exited (code 0x{:08X}) — shutting down", code));
						return true;
					};
					while (!quit.load()) {
						if (!SendHeartbeatIfDue()) break;
						const DWORD wait = ::MsgWaitForMultipleObjectsEx(
							2, waits, AnyRevealPending() ? 50 : 1000,
							QS_ALLINPUT, MWMO_INPUTAVAILABLE);
						if (wait == WAIT_OBJECT_0 + 1) {
							captureGameExit(0);
							break;
						}
						if (pipeDead.load()) {
							if (!captureGameExit(1000)) {
								log.Info("pipe closed while game remained active — shutting down");
							}
							break;
						}
						if (gameTopLevel && !::IsWindow(gameTopLevel)) {
							const auto now = ::GetTickCount64();
							if (gameWindowMissingSince == 0) {
								gameWindowMissingSince = now;
								log.Warn("game window disappeared before the process/pipe watchers fired");
							}
							struct FindCtx { DWORD pid; HWND found; } findCtx{
								::GetProcessId(gameProcess), nullptr };
							if (findCtx.pid != 0) {
								::EnumWindows([](HWND a_hwnd, LPARAM a_param) -> BOOL {
									auto& ctx = *reinterpret_cast<FindCtx*>(a_param);
									DWORD pid = 0;
									::GetWindowThreadProcessId(a_hwnd, &pid);
									if (pid != ctx.pid || !::IsWindowVisible(a_hwnd) ||
										::GetWindow(a_hwnd, GW_OWNER) != nullptr) {
										return TRUE;
									}
									ctx.found = a_hwnd;
									return FALSE;
								}, reinterpret_cast<LPARAM>(&findCtx));
							}
							if (findCtx.found) {
								log.Warn(std::format(
									"game window was recreated — re-attached (hwnd={:#x})",
									reinterpret_cast<std::uintptr_t>(findCtx.found)));
								gameTopLevel = findCtx.found;
								gameWindowMissingSince = 0;
							} else {
								constexpr std::uint64_t kMissingWindowGraceMs = 3000;
								if (now - gameWindowMissingSince >= kMissingWindowGraceMs) {
									if (!captureGameExit(1000)) {
										log.Info("game window remained absent for 3s while the process "
												 "handle still appeared active — shutting down");
									}
									break;
								}
							}
						} else {
							gameWindowMissingSince = 0;
						}
						DrainGameMessages();
						TickReveals();
						MSG message{};
						// Focus callbacks can post more window messages. Keep servicing native
						// commands (especially focus revocation) even while that queue stays busy.
						for (std::size_t dispatched = 0; dispatched < 64 &&
							::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++dispatched) {
							::TranslateMessage(&message);
							::DispatchMessageW(&message);
						}
					}
				} else {
					exitCode = 5;
				}

				Send(msg::ToJson(msg::Bye{
					.reason = !byeReason.empty() ? byeReason
						: exitCode == 0          ? "shutdown"
												 : "init-failed" }));
				log.pipe.store(nullptr, std::memory_order_release);
				CloseWebResources();
				if (dispatcher) {
					try { dispatcher.ShutdownQueueAsync(); } catch (...) {}
					dispatcher = nullptr;
				}
				rootVisual = nullptr;
				compositor = nullptr;
				captureDevice = nullptr;
				produceFence.Reset();
				context4.Reset();
				context.Reset();
				device5.Reset();
				device.Reset();
				if (hostWindow) {
					::DestroyWindow(hostWindow);
					hostWindow = nullptr;
				}
				if (bootstrapWindow) {
					::DestroyWindow(bootstrapWindow);
					bootstrapWindow = nullptr;
				}
				pipe.Close();
				if (reader.joinable()) reader.join();
				log.Info(std::format("browser host exiting (code {})", exitCode));
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

		// Production permits exactly one browser host per game process.
		const auto mutexName =
			std::format(L"Local\\osfui-wv2-host-{}", a_options.gamePid);
		const HANDLE instanceMutex = ::CreateMutexW(nullptr, TRUE, mutexName.c_str());
		if (!instanceMutex || ::GetLastError() == ERROR_ALREADY_EXISTS) {
			app.log.Error("another browser-host instance is already running for this game pid");
			return 3;
		}

		app.pipe.PrepareForOpen();
		if (!app.pipe.Connect(a_options.pipeName, 15000)) {
			app.log.Error("pipe connect failed: " + app.pipe.LastErrorText());
			::CloseHandle(instanceMutex);
			return 2;
		}
		const auto serverPid = app.pipe.ServerProcessId();
		if (!serverPid || *serverPid != a_options.gamePid) {
			app.log.Error(std::format(
				"rejected pipe server: expected game pid {}, kernel reported {}",
				a_options.gamePid, serverPid.value_or(0)));
			app.pipe.Close();
			::CloseHandle(instanceMutex);
			return 6;
		}
		app.log.Info(std::format("verified pipe server pid {}", *serverPid));
		app.log.pipe.store(&app.pipe, std::memory_order_release);

		app.gameProcess = ::OpenProcess(
			PROCESS_DUP_HANDLE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
			FALSE, a_options.gamePid);
		if (!app.gameProcess) {
			const auto error = ::GetLastError();
			auto message = std::format("OpenProcess(game pid {}) failed ({})",
				a_options.gamePid, error);
			if (error == ERROR_ACCESS_DENIED) {
				message += std::format(
					" — access denied: the game is likely running elevated (as "
					"administrator) while this browser host is not (elevated={}); run the "
					"game/MO2 without administrator rights",
					elevated ? "yes" : "no");
			}
			app.log.Error(message);
			::CloseHandle(instanceMutex);
			return 4;
		}

		LPWSTR webView2RuntimeVersionRaw = nullptr;
		std::string webView2RuntimeVersion = "unknown";
		if (SUCCEEDED(::GetAvailableCoreWebView2BrowserVersionString(nullptr, &webView2RuntimeVersionRaw)) &&
			webView2RuntimeVersionRaw) {
			webView2RuntimeVersion = ToUtf8(webView2RuntimeVersionRaw);
			::CoTaskMemFree(webView2RuntimeVersionRaw);
		} else {
			PromptInstallWebView2Runtime(app.log);
		}
		if (!app.Send(osfui::wv2::msg::ToJson(osfui::wv2::msg::Hello{
				.protocolVersion = kBrowserHostProtocolVersion,
				.hostVersion = OSFUI::kOsfuiReleaseVersion,
				.runtimeVersion = webView2RuntimeVersion,
				.pid = ::GetCurrentProcessId(),
			}))) {
			app.log.Error("hello write failed: " + app.pipe.LastErrorText());
			app.log.pipe.store(nullptr, std::memory_order_release);
			app.pipe.Close();
			::CloseHandle(app.gameProcess);
			::CloseHandle(instanceMutex);
			return 7;
		}
		app.log.Info("hello sent (WebView2 Runtime " + webView2RuntimeVersion + ")");

		int code = 10;
		try {
			app.wakeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!app.wakeEvent) {
				throw std::runtime_error("CreateEvent(wake) failed");
			}
			app.reader = std::thread([&app] {
				try {
					app.ReaderMain();
				} catch (const winrt::hresult_error& e) {
					app.log.Error("pipe reader failed: " + ToUtf8(e.message()));
					app.quit.store(true);
					::SetEvent(app.wakeEvent);
				} catch (const std::exception& e) {
					app.log.Error(std::string("pipe reader failed: ") + e.what());
					app.quit.store(true);
					::SetEvent(app.wakeEvent);
				} catch (...) {
					app.log.Error("pipe reader failed with an unknown exception");
					app.quit.store(true);
					::SetEvent(app.wakeEvent);
				}
			});
			code = app.Run();
		} catch (const winrt::hresult_error& e) {
			app.log.Error("unhandled browser-host failure: " + ToUtf8(e.message()));
		} catch (const std::exception& e) {
			app.log.Error(std::string("unhandled browser-host failure: ") + e.what());
		} catch (...) {
			app.log.Error("unhandled browser-host failure: unknown exception");
		}
		app.quit.store(true);
		if (app.wakeEvent) {
			::SetEvent(app.wakeEvent);
		}
		app.log.pipe.store(nullptr, std::memory_order_release);
		app.pipe.Close();
		if (app.reader.joinable()) {
			app.reader.join();
		}
		if (app.gameProcess) {
			::CloseHandle(app.gameProcess);
		}
		if (app.wakeEvent) {
			::CloseHandle(app.wakeEvent);
		}
		::CloseHandle(instanceMutex);
		return code;
	}
}
