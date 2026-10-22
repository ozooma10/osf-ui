#include "HostApp.h"
#include "EmbeddedScripts.h"
#include "FormControls.h"
#include "SpatialAudio.h"
#include "MediaNavigation.h"
#include "DocumentNavigation.h"
#include "CaptureTiming.h"
#include "CapturePerformance.h"

#include "Core/Ids.h"
#include "Core/Json.h"
#include "Render/BrowserHostRecovery.h"
#include "BrowserBoundedQueue.h"
#include "BrowserLocalUri.h"
#include "BrowserContentPolicy.h"
#include "BrowserSurface.h"
#include "BrowserMessages.h"
#include "Wv2CdpInput.h"
#include "BrowserMouseButtons.h"
#include "BrowserPipe.h"
#include "BrowserPipeWriter.h"
#include "BrowserProtocol.h"
#include "Win32Util.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <DispatcherQueue.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include "LanguageTag.h"
#include <wrl.h>
#include <wrl/client.h>
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

static_assert(osfui::webview2::MediaNavigation::kConnectionAborted == COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_ABORTED);

namespace osfui::webview2
{
	using namespace osfui::browser;
	namespace
	{
		using osfui::win32::ToUtf8;
		using osfui::win32::ToWide;

		namespace Json = OSFUI::Json;

		namespace msg = osfui::browser::msg;

		struct Logger
		{
			PipeWriter&   outbound; // forwarding never performs pipe I/O on the logging thread
			std::ofstream file;
			std::mutex    mutex;

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
				outbound.Enqueue(Json::Dump(msg::ToJson(msg::Log{ .level = a_level, .text = a_text })));
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

		struct App
		{
			Pipe        pipe;
			HANDLE      gameProcess{ nullptr };
			HANDLE      wakeEvent{ nullptr };
			std::atomic_bool pipeDead{ false };
			PipeWriter outbound{ pipe, [this] {
				pipeDead.store(true, std::memory_order_release);
				if (wakeEvent) ::SetEvent(wakeEvent);
			} };
			Logger      log{ outbound };
			std::thread reader;
			std::atomic_bool quit{ false };
			bool             rendererFatal{ false };  // STA thread only; first failure wins
			std::string      byeReason;  // overrides the default bye reason (STA thread only); first reason wins

			~App()
			{
				StopTransport();
				if (gameProcess) ::CloseHandle(gameProcess);
				if (wakeEvent) ::CloseHandle(wakeEvent);
			}

			// STA thread only.
			void RequestQuit(std::string_view a_reason)
			{
				if (byeReason.empty()) byeReason = a_reason;
				quit.store(true);
				if (wakeEvent) ::SetEvent(wakeEvent);
			}

			static constexpr std::size_t kMaxGameMessages = 1024;
			static constexpr std::size_t kGameMessagesPerDrain = 128;
			BoundedQueue<json> gameMessages{ kMaxGameMessages };
			std::uint64_t nextHeartbeatAt{ 0 };  // STA thread only

			bool                  initialized{ false };
			HWND                  gameTopLevel{ nullptr };
			std::filesystem::path viewsRoot, legacyViewsRoot, userData;
			std::uint32_t         width{ 1 }, height{ 1 };
			std::uint32_t         viewportWidth{ 1 }, viewportHeight{ 1 };
			bool                  devMode{ false };
			bool                  remoteMode{ false };
			std::string           language;  // game language code from init; empty keeps the WebView2 default
			bool                  windowActive{ true };

			HWND bootstrapWindow{ nullptr };
			HWND hostWindow{ nullptr };

			struct View
			{
				std::string id;
				std::uint64_t generation{ 0 };
				std::wstring modId;
				std::wstring viewName;
				HWND        window{ nullptr };
				winrt::Windows::UI::Composition::ContainerVisual visual{ nullptr };
				ComPtr<ICoreWebView2Controller>            controller;
				ComPtr<ICoreWebView2CompositionController> compositionController;
				ComPtr<ICoreWebView2>                      webView;
				std::unique_ptr<SpatialAudio> audio;
				unsigned audioRestartAttempts{};
				std::uint64_t nextAudioRestart{};
				ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> consoleReceiver;
				ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> exceptionReceiver;
				bool controllerRequested{ false };
				bool securityReady{ false };
				bool hidden{ true };
				bool newWindowBlockedWarned{ false };
				bool navigationBlockedWarned{ false };
				bool frameNavigationBlockedWarned{ false };
				std::uint64_t pageMessageWindowStarted{ 0 };
				std::uint32_t pageMessagesThisWindow{ 0 };
				bool pageMessageTooLargeWarned{ false };
				bool pageMessageFloodWarned{ false };
				std::uint32_t logicalHeight{ kDefaultLogicalHeight };
				bool legacy{};
				bool remoteContent{}; // Set before controller creation; immutable.
				bool browserLoading{}, browserLoadSucceeded{};
				MediaNavigation mediaNavigation;
				DocumentNavigation documentNavigation;
				std::unique_ptr<CapturePerformance> performance;
				bool mediaQueryInFlight{}; // At most one outstanding metadata query per controller.
				std::string lastBrowserState;
				json lastAudioState;
				int mouseX{}, mouseY{};
				bool          revealPending{ false };
				std::uint64_t revealDeadline{ 0 };
				std::string   revealToken;
				std::uint64_t pendingPresentationEpoch{ 0 };
				int  order{ 0 };
				bool domSeen{ false };
				bool captureWarm{ false }; // first launcher capture since navigation has been published
				std::wstring currentUrl;
				std::optional<std::wstring> pendingNavigate;
				std::deque<std::string> queuedPostWeb;
				std::size_t queuedPostWebBytes{ 0 };
				bool queuedPostWebOverflowWarned{ false };
				SyntheticMouseButtons syntheticMouseButtons;
				std::shared_ptr<CdpInputQueue> cdpInput;
				CdpPressedKeys cdpKeys;
				std::optional<bool> cdpFocused;
				bool cdpComposition{ false };
			};
			std::vector<std::unique_ptr<View>> views;  // creation order (= z tie-break)
			std::optional<msg::BrowserLayout> browserLayout;
			BrowserPointerRouter browserPointer;
			View* pointerTarget{};
			std::uint64_t nextViewGeneration{ 1 };
			View* inputTarget{ nullptr };  // mouse/focus/synthetic-key target
			bool  captureStarted{ false };
			bool          focusGranted{ false };
			bool          pointerInputEnabled{ true };
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
			// Capture callbacks, ring publication and game-message handling share the host STA.
			bool              captureClosing{ true };
			unsigned          captureCallbackFailures{};
			bool              captureHasVisibleView{ false };
			std::uint32_t      captureCadenceHz{ 0 };
			CaptureTiming     captureTiming;

			struct Slot
			{
				ComPtr<ID3D11Texture2D> texture;
				HANDLE                  localHandle{ nullptr };
				std::uint64_t           lastSerial{ 0 };
			};
			std::array<Slot, kRingSlots> ring{};
			std::uint32_t ringWidth{ 0 }, ringHeight{ 0 };
			std::uint32_t ringWrite{ 0 };
			ComPtr<ID3D11Fence> produceFence;
			std::uint64_t              frameSerial{ 0 };
			std::uint32_t              lastSlot{ 0 };
			std::uint64_t              presentationEpoch{ 0 };
			std::array<std::uint64_t, kRingSlots> ackedSerials{};
			std::uint64_t consumeLagDrops{ 0 };
			ComPtr<ID3D11Texture2D> pendingCapture; // one owned latest-wins copy
			std::uint64_t pendingCaptureEpoch{ 0 };
			struct CaptureOpening
			{
				std::string view;
				std::uint64_t epoch{ 0 };
				std::uint64_t settleUntil{ 0 };
			} captureOpening;

			ComPtr<ICoreWebView2Environment> environment;
			std::filesystem::path audioExtensionPath;
			bool audioExtensionRequested{}, audioExtensionReady{}, audioExtensionFailed{}, browserAudioMuted{};
			std::uint64_t audioExtensionDeadline{}, audioSceneExpires{};
			json audioScene;
			bool environmentRequested{ false };

			bool Send(const json& a_msg, std::unique_ptr<PipeWriter::HandleBatch> a_handles = {})
			{
				if (Json::Get(a_msg, "type", "") == msg::Console::kType &&
					Json::Get(a_msg, "json", "").size() > 1024u * 1024u) {
					log.Warn("dropping console message larger than 1 MiB");
					return false;
				}
				const auto result = outbound.Enqueue(Json::Dump(a_msg), std::move(a_handles));
				if (result == PipeWriter::Result::Queued) return true;
				if (result == PipeWriter::Result::InvalidPayload) {
					log.Warn("dropping oversized outbound message");
					return false;
				}
				pipeDead.store(true, std::memory_order_release);
				captureClosing = true;
				RequestQuit(result == PipeWriter::Result::Full ? "pipe-queue-full" : "pipe-write");
				return false;
			}

			void StopTransport(std::chrono::milliseconds a_drain = std::chrono::milliseconds::zero())
			{
				gameMessages.Close();
				outbound.Stop(a_drain); // Cancels blocked reads/writes before either thread is joined.
				if (reader.joinable()) reader.join();
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
					const auto coalesceKey = GameMessageCoalesceKey(type,
						Json::Get(parsed, "kind", ""),
						Json::Get(parsed, "view", ""));
					const auto result =
						gameMessages.Push(std::move(parsed), coalesceKey);
					if (result == decltype(gameMessages)::PushResult::Full) {
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
#include "HostPerformance.inl"
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
			static constexpr std::uint64_t kColdOpenSettleMs = 100;

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
				const bool changed = std::exchange(captureHasVisibleView, anyVisible) != anyVisible;
				if (changed) ApplyCaptureCadence();
			}

			void HideView(View& a_view)
			{
				if (a_view.performance) a_view.performance->SetActive(false, CapturePerformance::Now());
				if (captureOpening.view == a_view.id) {
					captureOpening = {};
					pendingCaptureEpoch = 0;
				}
				if (a_view.hidden && !a_view.revealPending) return;
				RecoverPressedMouseButtons(a_view, "view hide");
				SetCdpFocus(a_view, false);
				a_view.hidden = true;
				RefreshCaptureVisibility();
				a_view.pendingPresentationEpoch = 0;
				a_view.revealPending = false;
				a_view.revealToken.clear();
				if (a_view.visual) a_view.visual.IsVisible(false);
				if (a_view.controller) a_view.controller->put_IsVisible(FALSE);
				log.Info(std::format("view '{}': hide", a_view.id));
			}

			void ShowView(View& a_view)
			{
				if (!a_view.hidden) {
					log.Info(std::format("view '{}': show - already visible (visual={})",
						a_view.id, a_view.visual && a_view.visual.IsVisible()));
					if (captureOpening.view == a_view.id) {
						PromoteChangedPresentation(a_view); // require a fresh capture even when already shown
					} else if (PromotePresentation(a_view)) {
						RepublishLatest();
					}
					if (focusGranted && inputTarget == &a_view && a_view.controller) {
						ReconcileCdpFocus();
					}
					return;
				}
				a_view.hidden = false;
				RefreshCaptureVisibility();
				if (a_view.controller) a_view.controller->put_IsVisible(TRUE);
				ReconcileCdpFocus();
				if (!a_view.remoteContent && a_view.visual && a_view.webView && a_view.domSeen) {
					a_view.revealPending = true;
					a_view.revealToken = NewRevealToken();
					a_view.revealDeadline = ::GetTickCount64() + kRevealTimeoutMs;
					log.Info(std::format("view '{}': show - reveal pending ({} ms timeout)",
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
						"view '{}': show - direct (visual={} webView={} domSeen={})", a_view.id,
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
						"view '{}': reveal sentinel timed out - showing anyway", a_view.id));
				} else {
					log.Info(std::format("view '{}': reveal sentinel arrived - showing", a_view.id));
				}
				if (a_view.visual && !a_view.hidden) a_view.visual.IsVisible(true);
				PromoteChangedPresentation(a_view);
			}

			void TickReveals()
			{
				const auto now = ::GetTickCount64();
				if (audioExtensionRequested && !audioExtensionReady && !audioExtensionFailed &&
					osfui::browser::audio::ClockMs() >= audioExtensionDeadline)
					FailAudioExtension("Browser audio extension installation timed out");
				for (auto& view : views) {
					TickMediaNavigation(*view, now);
					if (view->audio) TickAudio(*view);
					if (view->cdpInput) view->cdpInput->CheckTimeout();
					if (view->revealPending && now >= view->revealDeadline) {
						CompleteReveal(*view, /*a_timedOut=*/true);
					}
				}
			}

			void DestroyOneView(View& a_view)
			{
				StopPerformance(a_view, "view destroyed");
				a_view.mediaNavigation.Start(0);
				a_view.documentNavigation = {};
				a_view.audio.reset(); // Mute source before closing the collector or source controller.
				if (pointerTarget == &a_view) { pointerTarget=nullptr; browserPointer.Reset(); }
				if (captureOpening.view == a_view.id) {
					captureOpening = {};
					pendingCaptureEpoch = 0;
				}
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

			void FailHost(std::string_view a_stage, HRESULT a_hr, std::string_view a_description, std::string_view a_view = {})
			{
				captureClosing = true;
				if (!rendererFatal) {
					rendererFatal = true;
					log.Error(a_view.empty()
						? std::format("{}: {} (0x{:08X})", a_stage, a_description, static_cast<unsigned>(a_hr))
						: std::format("{} (view '{}'): {} (0x{:08X})", a_stage, a_view, a_description, static_cast<unsigned>(a_hr)));
					Send(msg::ToJson(msg::Fatal{ .stage = std::string(a_stage), .view = std::string(a_view),
						.description = std::string(a_description), .code = static_cast<std::uint32_t>(a_hr) }));
				}
				RequestQuit(a_stage);
			}

			bool BeginEnvironment()
			{
				if (environmentRequested) return true;
				environmentRequested = true;
				std::error_code ec;
				std::filesystem::create_directories(userData, ec);
				if (ec) {
					FailHost("environment", HRESULT_FROM_WIN32(ec.value()), "could not create WebView2 user data directory");
					return false;
				}
				const auto callback =
					Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
						[this](HRESULT a_hr, ICoreWebView2Environment* a_environment) -> HRESULT {
							if (quit.load()) return S_OK;
							if (FAILED(a_hr) || !a_environment) {
								FailHost("environment", FAILED(a_hr) ? a_hr : E_POINTER, "WebView2 environment callback failed");
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
					FailHost("environment-options", E_OUTOFMEMORY, "could not allocate WebView2 environment options");
					environmentRequested = false;
					return false;
				}
				auto browserArguments = WebViewBrowserArguments(remoteMode);
				if (remoteMode) {
					ComPtr<ICoreWebView2EnvironmentOptions6> extensions;
					if (FAILED(environmentOptions.As(&extensions)) ||
						FAILED(extensions->put_AreBrowserExtensionsEnabled(TRUE))) {
						FailAudioExtension("WebView2 browser extensions are unavailable");
					} else {
						try { audioExtensionPath = PrepareAudioExtension(userData); }
						catch (const std::exception& error) { FailAudioExtension(error.what()); }
						browserArguments += std::wstring(L" --allowlisted-extension-id=") + kAudioExtensionId +
							L" --disable-background-timer-throttling";
					}
				}
				const auto optionsHr = environmentOptions->put_AdditionalBrowserArguments(
					browserArguments.c_str());
				if (FAILED(optionsHr)) {
					FailHost("environment-options", optionsHr, "WebView2 browser arguments rejected");
					environmentRequested = false;
					return false;
				}
				log.InfoFwd(
					"WebView2 renderer and native-occlusion throttling disabled for the offscreen capture browser host");
				if (const auto tag = LanguageTag::FromGameLanguage(language); !tag.empty()) {
					// Drives navigator.language, Accept-Language and Chromium's own UI strings for every view.
					const auto languageHr = environmentOptions->put_Language(ToWide(tag).c_str());
					if (FAILED(languageHr)) {
						log.Warn(std::format("WebView2 rejected language '{}' (hr=0x{:08X}); keeping its default", tag, static_cast<std::uint32_t>(languageHr)));
					} else {
						log.Info(std::format("WebView2 language '{}' (game language '{}')", tag, language));
					}
				} else if (!language.empty()) {
					log.Warn(std::format("game language '{}' has no BCP-47 mapping; keeping the WebView2 default", language));
				}
				const auto hr = ::CreateCoreWebView2EnvironmentWithOptions(
					nullptr, userData.c_str(), environmentOptions.Get(), callback.Get());
				if (FAILED(hr)) {
					FailHost("environment", hr, "CreateCoreWebView2EnvironmentWithOptions failed");
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
					FailHost("composition-controller", E_NOINTERFACE, "composition controller API unavailable", a_view.id);
					return;
				}
				a_view.controllerRequested = true;
				const auto hr = environment3->CreateCoreWebView2CompositionController(
					a_view.window,
					Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
						[this, id = a_view.id, generation = a_view.generation](HRESULT a_controllerHr,
							ICoreWebView2CompositionController* a_controller) -> HRESULT {
							if (auto* view = FindView(id); view && view->generation == generation) {
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
					FailHost("composition-controller", hr, "CreateCompositionController failed", a_view.id);
					a_view.controllerRequested = false;
				}
			}

			void ReportControllerFailure(View& a_view, HRESULT a_hr,
				std::string_view a_description)
			{
				FailHost("composition-controller", a_hr, a_description, a_view.id);
			}

			void ReportSecurityFailure(View& a_view, HRESULT a_hr,
				std::string_view a_description)
			{
				if (!rendererFatal && a_view.webView) a_view.webView->Stop();
				FailHost("browser-security", a_hr, a_description, a_view.id);
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
				const bool allowed = IsAllowedContentDocument(a_view.remoteContent, uri, a_view.modId, a_view.viewName, a_frame);
				if (allowed) {
					if (!a_frame) {
						UINT64 navigationId{};
						a_args->get_NavigationId(&navigationId);
						StopPerformance(a_view, "navigation");
						a_view.documentNavigation.Start(navigationId);
						a_view.mediaNavigation.Start(navigationId);
						if (a_view.audio) a_view.audio->SetLoading(true);
						a_view.audioRestartAttempts = 0;
						a_view.browserLoading=true; a_view.browserLoadSucceeded=false;
						PublishBrowserState(a_view);
						RecoverPressedMouseButtons(a_view, "main-frame navigation");
						a_view.domSeen = false;
						a_view.captureWarm = false;
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

#include "MediaNavigation.inl"

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

#include "RemoteContent.inl"
#include "AudioRouting.inl"
#include "BrowserSurface.inl"

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
				if (remoteMode && (a_view.id == kBrowserView || a_view.id == kBrowserContent)) {
					a_view.audio = std::make_unique<SpatialAudio>(a_view.webView.Get(),
						[this, id = a_view.id](const std::string& error) { log.Warn(std::format("view '{}': {}", id, error)); });
					if (!a_view.audio->Guarded()) {
						ReportSecurityFailure(a_view, E_FAIL, "browser audio mute guard could not be installed");
						return S_OK;
					}
					a_view.audio->Update(audioScene, audioSceneExpires);
					a_view.audio->SetMuted(browserAudioMuted);
					a_view.audio->SetLoading(true);
				}
				ComPtr<ICoreWebView2Settings> settings;
				if (SUCCEEDED(a_view.webView->get_Settings(&settings)) && settings) {
					settings->put_AreDefaultContextMenusEnabled(FALSE);
					settings->put_AreDevToolsEnabled(devMode ? TRUE : FALSE);
					settings->put_IsZoomControlEnabled(FALSE);
				} else {
					log.Warn(std::format("view '{}': settings unavailable - the browser "
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
					FailHost("composition-target", result, "put_RootVisualTarget failed", a_view.id);
					return S_OK;
				}
				if (a_view.remoteContent) {
					result = InstallRemoteContentPolicy(a_view);
					if (SUCCEEDED(result)) result = InstallBrowserStateEvents(a_view);
					if (FAILED(result)) {
						ReportSecurityFailure(a_view, result, "remote-content policy installation failed");
						return S_OK;
					}
				} else {
					ComPtr<ICoreWebView2_3> webView3;
					if (FAILED(a_view.webView.As(&webView3))) {
						ReportSecurityFailure(a_view, E_NOINTERFACE, "virtual-host mapping API unavailable");
						return S_OK;
					}
					const auto& assetRoot = a_view.legacy ? legacyViewsRoot : viewsRoot;
					result = webView3->SetVirtualHostNameToFolderMapping(kViewHost.data(), assetRoot.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
					if (FAILED(result)) {
						ReportSecurityFailure(a_view, result, "OSF UI virtual-host mapping failed");
						return S_OK;
					}
				}
				ApplyViewLayout();
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
				InitializeAudio(a_view);
				if (!captureStarted) {
					if (!StartCapture()) return;
					captureStarted = true;
				}
				log.InfoFwd(std::format("view '{}': controller ready ({} view(s) hosted)",
					a_view.id, views.size()));
				DrainQueuedViewWork(a_view);
				ReconcileCdpFocus();
			}

			void InstallPerformanceOverlay(View& a_view)
			{
				if (!devMode) { FinishControllerSetup(a_view); return; }
				const auto hr = AddDocumentScript(a_view, EmbeddedScript::PerformanceOverlay,
					[this, id = a_view.id, generation = a_view.generation](HRESULT result) {
						auto* view = FindView(id);
						if (!view || view->generation != generation) return;
						if (FAILED(result)) log.Warn("Performance overlay installation failed");
						FinishControllerSetup(*view);
					});
				if (FAILED(hr)) {
					log.Warn("Performance overlay registration failed");
					FinishControllerSetup(a_view);
				}
			}

			void InstallBridgeShim(View& a_view)
			{
				const auto hr = AddDocumentScript(a_view,
					a_view.legacy ? EmbeddedScript::LegacyBridgeShim : EmbeddedScript::BridgeShim,
					[this, id = a_view.id, generation = a_view.generation](const HRESULT a_scriptHr) {
						auto* view = FindView(id);
						if (!view || view->generation != generation) return;
						if (FAILED(a_scriptHr)) {
							FailHost("bridge-shim", a_scriptHr, "bridge shim installation failed");
						} else {
							// Includes form-control styles for every document and iframe.
							InstallPerformanceOverlay(*view);
						}
					});
				if (FAILED(hr)) {
					FailHost("bridge-shim", hr, "bridge shim registration failed");
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
				const std::vector<const wchar_t*> patterns = a_view.remoteContent ?
					std::vector<const wchar_t*>{ L"*" } : std::vector<const wchar_t*>{ L"http://*", L"https://*" };
				for (const auto* pattern : patterns) {
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
							if (IsAllowedContentResource(view->remoteContent, uri)) return S_OK;
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
									log.Warn(std::format("view '{}': {} distinct blocked origins - further " "egress denials for this view are silent", view->id, kMaxEgressWarnsPerView));
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
				if (a_view.remoteContent) {
					// Remote pages receive neither the bridge nor the offline
					// transport shim. All native policy must be installed first.
					FinishControllerSetup(a_view);
					return S_OK;
				}
				const auto viewId = a_view.id;
				const auto generation = a_view.generation;
				const auto scriptHr = AddDocumentScript(a_view, EmbeddedScript::NetworkGuard,
					[this, viewId, generation](const HRESULT a_scriptHr) {
						auto* current = FindView(viewId);
						if (!current || current->generation != generation) return;
						if (FAILED(a_scriptHr)) {
							log.Error(std::format("egress neuter script install failed (0x{:08X})",
								static_cast<unsigned>(a_scriptHr)));
							ReportSecurityFailure(*current, a_scriptHr,
								"egress transport policy installation failed");
						} else {
							InstallBridgeShim(*current);
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

			void QueueGameFocusRestore()
			{
				if (gameTopLevel) {
					if (const auto message = RestoreGameFocusMessage()) {
						::PostMessageW(gameTopLevel, message, 0, 0);
					}
				}
			}

			void InstallEvents(View& a_view)
			{
				View* view = &a_view;
				EventRegistrationToken token{};
				a_view.compositionController->add_CursorChanged(
					Callback<ICoreWebView2CursorChangedEventHandler>(
						[this, view](ICoreWebView2CompositionController* a_sender, ::IUnknown*) -> HRESULT {
							// Only the input-target view drives the real OS pointer.
							if (view != (browserLayout ? pointerTarget : inputTarget)) return S_OK;
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
							if (windowActive) QueueGameFocusRestore();
							return S_OK;
						}).Get(), &token);
				a_view.webView->add_WebMessageReceived(
					Callback<ICoreWebView2WebMessageReceivedEventHandler>(
						[this, view](ICoreWebView2*,
							ICoreWebView2WebMessageReceivedEventArgs* a_args) -> HRESULT {
							if (view->remoteContent) return S_OK; // No parsing, logging, forwarding or FailHost.
							constexpr std::size_t kMaxPageMessageChars = kMaxPageMessageBytes;
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
							if (HandlePerformanceMessage(*view, text)) return S_OK;
							Send(msg::ToJson(msg::WebMessage{ .view = view->id,
								.json = std::move(text) }));
							return S_OK;
						}).Get(), &token);
				if (!a_view.remoteContent) a_view.webView->add_NewWindowRequested(
					Callback<ICoreWebView2NewWindowRequestedEventHandler>(
						[this, view](ICoreWebView2*,
							ICoreWebView2NewWindowRequestedEventArgs* a_args) -> HRESULT {
							a_args->put_Handled(TRUE);
							if (!view->newWindowBlockedWarned) {
								view->newWindowBlockedWarned = true;
								log.Warn(std::format("view '{}': blocked external new-window request", view->id));
							}
							return S_OK;
						}).Get(), &token);
				a_view.webView->add_NavigationCompleted(
					Callback<ICoreWebView2NavigationCompletedEventHandler>(
						[this, view](ICoreWebView2* a_sender,
							ICoreWebView2NavigationCompletedEventArgs* a_args) -> HRESULT {
							BOOL success = FALSE;
							COREWEBVIEW2_WEB_ERROR_STATUS status{};
							a_args->get_IsSuccess(&success);
							a_args->get_WebErrorStatus(&status);
							UINT64 navigationId{};
							a_args->get_NavigationId(&navigationId);
							int httpStatus = -1;
							ComPtr<ICoreWebView2NavigationCompletedEventArgs2> args2;
							if (SUCCEEDED(a_args->QueryInterface(IID_PPV_ARGS(&args2))))
								args2->get_HttpStatusCode(&httpStatus);
							// HTTP success alone does not imply completed navigation: a response
							// may be followed by a connection abort. Keep both results in evidence.
							log.Info(std::format(
								"view '{}': navigation {} completed (success={}, webErrorStatus={}, httpStatus={})",
								view->id, navigationId, success == TRUE, static_cast<int>(status), httpStatus));
							if (!view->documentNavigation.Current(navigationId) || !view->documentNavigation.loading) return S_OK;
							// Settle the current cancellation without a recovery failure; a
							// superseded navigation must not change the replacement's state.
							if (status == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) {
								CancelNavigation(*view, navigationId);
								return S_OK;
							}
							LPWSTR raw = nullptr;
							std::string source;
							if (SUCCEEDED(a_sender->get_Source(&raw)) && raw) source = ToUtf8(raw);
							::CoTaskMemFree(raw);
							const auto now = ::GetTickCount64();
							if (view->mediaNavigation.Begin(view->remoteContent, success == TRUE,
								static_cast<int>(status), httpStatus, navigationId, source, now)) {
								// Direct media can finish its DOM after the document request aborts.
								// Keep source output guarded while verifying the committed document.
								TickMediaNavigation(*view, now);
								return S_OK;
							}
							CompleteNavigation(*view, success == TRUE, static_cast<int>(status), std::move(source));
							return S_OK;
						}).Get(), &token);
				a_view.webView->add_ContentLoading(
					Callback<ICoreWebView2ContentLoadingEventHandler>(
						[view](ICoreWebView2*, ICoreWebView2ContentLoadingEventArgs* args) -> HRESULT {
							UINT64 navigationId{};
							BOOL errorPage{};
							args->get_NavigationId(&navigationId);
							args->get_IsErrorPage(&errorPage);
							view->documentNavigation.Commit(navigationId, errorPage == TRUE);
							return S_OK;
						}).Get(), &token);
				ComPtr<ICoreWebView2_2> webView2;
				if (SUCCEEDED(a_view.webView.As(&webView2))) {
					webView2->add_DOMContentLoaded(
						Callback<ICoreWebView2DOMContentLoadedEventHandler>(
							[this, view](ICoreWebView2*, ICoreWebView2DOMContentLoadedEventArgs* args) -> HRESULT {
								UINT64 navigationId{};
								args->get_NavigationId(&navigationId);
								if (!view->documentNavigation.DomReady(navigationId)) return S_OK;
								view->mediaNavigation.DomReady(navigationId);
								view->domSeen = true;
								InitializeCdpInput(*view);
								DrainQueuedViewWork(*view);
								TickMediaNavigation(*view, ::GetTickCount64());
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
							if (view->audio) view->audio->Fail("Source browser process failed");
							switch (kind) {
							case COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED:
								RequestQuit("browser-process-exited");
								break;
							case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED:
							case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE:
								view->domSeen = false;
								view->mediaNavigation.Start(0);
								view->documentNavigation = {};
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

			void EnableCaptureTiming()
			{
				const auto timing = captureTiming.Enable();
				if (timing.timer != TIMERR_NOERROR || timing.policyError) {
					log.Warn(std::format("WGC timing guard partially unavailable (timer={}, policy={})",
						timing.timer, timing.policyError));
				} else {
					log.Info("WGC timing guard active: 1 ms resolution, occlusion throttling disabled");
				}
			}

			void ApplyCaptureCadence()
			{
				if (!captureSession) return;
				const bool visible = captureHasVisibleView;
				const std::uint32_t desiredHz = visible ? 60u : 4u;
				if (captureCadenceHz == desiredHz) return;
				if (visible) {
					EnableCaptureTiming();
				} else if (const auto error = captureTiming.Disable()) {
					log.Warn(std::format("WGC timing policy restore failed ({})", error));
				}
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
							visible ? "visible" : "hidden",
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
				captureClosing = false;
				try {
					using namespace winrt::Windows::Graphics;
					using namespace winrt::Windows::Graphics::Capture;
					using namespace winrt::Windows::Graphics::DirectX;
					captureItem = GraphicsCaptureItem::CreateFromVisual(rootVisual);
					// InitializeComposition creates this STA's dispatcher; Run pumps its messages.
					framePool = Direct3D11CaptureFramePool::Create(
						captureDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 3,
						SizeInt32{ static_cast<std::int32_t>(width),
							static_cast<std::int32_t>(height) });
					frameToken = framePool.FrameArrived(
						[this](winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& a_pool,
							winrt::Windows::Foundation::IInspectable const&) {
							OnFrameArrived(a_pool);
						});
					captureSession = framePool.CreateCaptureSession(captureItem);
					try { captureSession.IsCursorCaptureEnabled(false); }
					catch (const winrt::hresult_error& a_error) {
						log.Warn(std::format("cursor capture control unavailable: {}", ToUtf8(a_error.message())));
					}
					ApplyCaptureCadence();
					captureSession.StartCapture();
					return true;
				} catch (const winrt::hresult_error& a_error) {
					FailHost("capture-setup", a_error.code(), ToUtf8(a_error.message()));
					return false;
				}
			}

			void OnFrameArrived(const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& a_pool)
			{
				if (quit.load() || captureClosing) return;
				try {
					const auto framePresentationEpoch = presentationEpoch;
					const auto capturedFrame = a_pool.TryGetNextFrame();
					if (!capturedFrame) return;
					RecordPerformance(0);
					// Drain captures while hidden so the WGC pool stays current.
					if (!captureHasVisibleView) return;
					if (!captureOpening.view.empty() &&
						(framePresentationEpoch != captureOpening.epoch || AnyRevealPending())) return;
					auto access = capturedFrame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
					ComPtr<ID3D11Texture2D> source;
					winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&source)));
					D3D11_TEXTURE2D_DESC desc{};
					source->GetDesc(&desc);
					PublishFrame(source.Get(), desc.Width, desc.Height, framePresentationEpoch);
					captureCallbackFailures = 0;
				} catch (const winrt::hresult_error& a_error) {
					// Preserve 2.0's tolerance of an isolated callback error. Device loss
					// needs immediate recovery; persistent unknown failures still surface.
					if (!OSFUI::BrowserHostRecovery::IsRetryable("capture-frame", static_cast<std::uint32_t>(a_error.code().value)) &&
						++captureCallbackFailures < 3) {
						log.Warn(std::format("dropping capture frame after callback error ({}/3): {}", captureCallbackFailures, ToUtf8(a_error.message())));
						return;
					}
					FailHost("capture-frame", a_error.code(),
						std::format("capture callback failed: {}", ToUtf8(a_error.message())));
				}
			}

			// Game-message handling; STA thread.

			void DrainQueuedViewWork(View& a_view)
			{
				// The controller arrives before the async network guard and FinishControllerSetup complete.
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
				if (a_view.remoteContent) return;
				for (auto& message : a_view.queuedPostWeb) {
					const auto wide = ToWide(message);
					a_view.webView->PostWebMessageAsString(wide.c_str());
				}
				a_view.queuedPostWeb.clear();
				a_view.queuedPostWebBytes = 0;
			}

			void ApplyScale(View& a_view)
			{
				if (!a_view.controller) return;
				ComPtr<ICoreWebView2Controller4> controller4;
				if (FAILED(a_view.controller.As(&controller4)) || !controller4) {
					log.Warn(std::format("view '{}': ICoreWebView2Controller4 unavailable - "
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
					const auto rect = ViewBounds(*view);
					if (view->visual) {
						view->visual.Size({ static_cast<float>(rect.width), static_cast<float>(rect.height) });
						view->visual.Offset({ offsetX + rect.x, offsetY + rect.y, 0.0f });
					}
					if (view->controller) {
						view->controller->put_Bounds(RECT{ 0, 0,
							(std::max)(1,rect.width), (std::max)(1,rect.height) });
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
				AdvanceChangedPresentation(a_presentationEpoch);
				log.Info(std::format(
					"content viewport -> {}x{} inside stable {}x{} capture",
					viewportWidth, viewportHeight, width, height));
			}

			void ApplyResize(std::uint32_t a_width, std::uint32_t a_height)
			{
				pendingCaptureEpoch = 0;
				if (captureOpening.settleUntil) captureOpening.settleUntil = ::GetTickCount64() + kColdOpenSettleMs;
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
				try {
					framePool.Recreate(captureDevice,
						winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
						3, winrt::Windows::Graphics::SizeInt32{
							static_cast<std::int32_t>(width), static_cast<std::int32_t>(height) });
				} catch (const winrt::hresult_error& a_error) {
					FailHost("capture-resize", a_error.code(),
						std::format("frame pool resize failed: {}", ToUtf8(a_error.message())));
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
							POINT{ a_view.mouseX, a_view.mouseY }) :
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
				browserPointer.Reset();
			}

			void SendMouse(const json& a_msg)
			{
				if (!focusGranted || !windowActive) return;
				if (!pointerInputEnabled) return;
				const auto mouse = msg::FromJson<msg::Mouse>(a_msg);
				View* mouseView = inputTarget;
				if (browserLayout) {
					const auto panel = browserPointer.Route(mouse.x, mouse.y, mouse.kind == "button",
						mouse.button, mouse.down,
						BrowserPanelRect(viewportWidth,viewportHeight,browserLayout->chromeHeight,browserLayout->fullscreen,browserLayout->landing,BrowserPanel::Chrome),
						BrowserPanelRect(viewportWidth,viewportHeight,browserLayout->chromeHeight,browserLayout->fullscreen,browserLayout->landing,BrowserPanel::Content));
					mouseView = panel == BrowserPanel::None ? nullptr :
						FindView(panel == BrowserPanel::Chrome ? browserLayout->chrome : browserLayout->content);
					if (pointerTarget != mouseView) {
						if (pointerTarget && pointerTarget->compositionController)
							pointerTarget->compositionController->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE,
								COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE,0,POINT{});
						pointerTarget = mouseView;
						UINT32 cursor = 32512;
						if (mouseView && mouseView->compositionController) mouseView->compositionController->get_SystemCursorId(&cursor);
						Send(msg::ToJson(msg::Cursor{.id=cursor}));
					}
					if (mouseView && mouse.kind == "button" && mouse.down) FocusBrowserView(mouseView);
				}
				if (!mouseView || mouseView->hidden || !mouseView->domSeen || !mouseView->compositionController) return;
				const std::string& kind = mouse.kind;
				const auto rect = ViewBounds(*mouseView);
				int x = mouse.x - rect.x;
				int y = mouse.y - rect.y;
				mouseView->mouseX=x; mouseView->mouseY=y;
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
				} else if (kind == "wheel") {
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
				auto buttons = mouseView->syntheticMouseButtons;
				if (trackedButton) buttons.Observe(*trackedButton, buttonDown);
				const auto virtualKeys = static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(
					static_cast<UINT32>(MouseVirtualKeys(buttons.Pressed())) | (mouse.modifiers & (MK_SHIFT | MK_CONTROL)));
				const auto hr = mouseView->compositionController->SendMouseInput(eventKind,
					virtualKeys, data, POINT{ x, y });
				if (SUCCEEDED(hr) && trackedButton) {
					mouseView->syntheticMouseButtons.Observe(*trackedButton, buttonDown);
				}
			}

#include "GameMessages.inl"

			void DrainGameMessages()
			{
				decltype(gameMessages)::Item item;
				std::size_t drained = 0;
				while (!quit.load() && drained < kGameMessagesPerDrain && gameMessages.TryPop(item)) {
					HandleGameMessage(item.value);
					++drained;
				}
				if (gameMessages.Size() != 0) ::SetEvent(wakeEvent);
			}

			void CloseWebResources()
			{
				focusGranted = false;
				captureClosing = true;
				if (framePool) {
					try { framePool.FrameArrived(frameToken); }
					catch (const winrt::hresult_error& e) {
						log.Warn(std::format("capture frame callback removal failed (hr=0x{:08X})",
							static_cast<std::uint32_t>(e.code().value)));
					}
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
				if (const auto error = captureTiming.Disable()) {
					log.Warn(std::format("WGC timing policy restore failed ({})", error));
				}
				if (framePool) {
					try { framePool.Close(); }
					catch (const winrt::hresult_error& e) {
						log.Warn(std::format("capture-pool close failed (hr=0x{:08X})",
							static_cast<std::uint32_t>(e.code().value)));
					}
					framePool = nullptr;
				}
				captureItem = nullptr;
				captureOpening = {};
				ReleaseRing();
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
				if (!Send(msg::ToJson(msg::Heartbeat{}))) {
					quit.store(true, std::memory_order_release);
					return false;
				}
				nextHeartbeatAt = now + kHeartbeatIntervalMs;
				return true;
			}
			int Run()
			{
				// Establish timing before constructing the dispatcher/compositor and
				// browser processes. Applying it only after capture starts is too late
				// to avoid coarse startup pacing. Visibility takes over once initialized.
				EnableCaptureTiming();
				winrt::init_apartment(winrt::apartment_type::single_threaded);
				int exitCode = 0;
				if (CreateWindows() && InitializeComposition()) {
					const HANDLE waits[2] = { wakeEvent, gameProcess };
					const auto captureGameExit = [this] {
						if (::WaitForSingleObject(gameProcess, 0) != WAIT_OBJECT_0) {
							return false;
						}
						DWORD code = 0;
						if (!::GetExitCodeProcess(gameProcess, &code) || code == STILL_ACTIVE) {
							return false;
						}
						log.Info(std::format(
							"game process exited (code 0x{:08X}) - shutting down", code));
						return true;
					};
					while (!quit.load()) {
						if (!SendHeartbeatIfDue()) break;
						DWORD waitMs = (AnyRevealPending() || remoteMode) ? 50 : 1000;
						const auto now = ::GetTickCount64();
						if (captureOpening.settleUntil > now) {
							waitMs = static_cast<DWORD>((std::min)(std::uint64_t{ waitMs }, captureOpening.settleUntil - now));
						}
						const DWORD wait = ::MsgWaitForMultipleObjectsEx(
							2, waits, waitMs,
							QS_ALLINPUT, MWMO_INPUTAVAILABLE);
						if (wait == WAIT_OBJECT_0 + 1) {
							captureGameExit();
							break;
						}
						if (pipeDead.load()) {
							if (!captureGameExit()) log.Info("game pipe closed - shutting down");
							break;
						}
						DrainGameMessages();
						if (quit.load()) break;
						TickReveals();
						MSG message{};
						// Limit callback processing so native commands can still revoke focus.
						for (std::size_t dispatched = 0; dispatched < 64 &&
							::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++dispatched) {
							::TranslateMessage(&message);
							::DispatchMessageW(&message);
						}
						// A static page may not produce another capture after its final paint.
						if (captureOpening.settleUntil) RetryPendingCapture();
					}
				} else {
					exitCode = 5;
				}

				if (outbound.Failed() == PipeWriter::Failure::Overflow) {
					log.Info("outbound pipe queue exceeded its message/byte limit - shutting down");
				} else if (outbound.Failed() == PipeWriter::Failure::Write) {
					log.Info("outbound pipe write failed: " + pipe.LastErrorText());
				}
				Send(msg::ToJson(msg::Bye{
					.reason = !byeReason.empty() ? byeReason
						: exitCode == 0          ? "shutdown"
												 : "init-failed" }));
				StopTransport(std::chrono::milliseconds{ 250 });
				CloseWebResources();
				if (dispatcher) {
					try { dispatcher.ShutdownQueueAsync(); }
					catch (const winrt::hresult_error& e) {
						log.Warn(std::format("dispatcher shutdown failed (hr=0x{:08X})",
							static_cast<std::uint32_t>(e.code().value)));
					}
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
				log.Info(std::format("browser host exiting (code {})", exitCode));
				return exitCode;
			}
		};
	}

	int RunHost(const HostOptions& a_options)
	{
		App app;
		app.log.Open(a_options.logFile);

		const bool elevated = osfui::win32::IsProcessElevated();
		const auto exePath = osfui::win32::ModulePath();
		app.log.Info(std::format(
			"osfui_webview2_host starting (pid {}, game pid {}, pipe '{}', elevated={}, exe '{}')",
			::GetCurrentProcessId(), a_options.gamePid, ToUtf8(a_options.pipeName),
			elevated ? "yes" : "no", ToUtf8(exePath.native())));

		app.pipe.PrepareForOpen();
		if (!app.pipe.Connect(a_options.pipeName, 15000)) {
			app.log.Error("pipe connect failed: " + app.pipe.LastErrorText());
			return 2;
		}

		app.gameProcess = ::OpenProcess(
			PROCESS_DUP_HANDLE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
			FALSE, a_options.gamePid);
		if (!app.gameProcess) {
			const auto error = ::GetLastError();
			auto message = std::format("OpenProcess(game pid {}) failed ({})", a_options.gamePid, error);
			if (error == ERROR_ACCESS_DENIED) {
				message += std::format("access denied: the game is likely running elevated (as administrator) while this browser host is not (elevated={}); run the game/MO2 without administrator rights",
					elevated ? "yes" : "no");
			}
			app.log.Error(message);
			return 4;
		}

		app.wakeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!app.wakeEvent) {
			app.log.Error(std::format("CreateEvent(wake) failed (Win32 error {})", ::GetLastError()));
			return 10;
		}
		app.outbound.Start();

		LPWSTR webView2RuntimeVersionRaw = nullptr;
		std::string webView2RuntimeVersion = "unknown";
		if (SUCCEEDED(::GetAvailableCoreWebView2BrowserVersionString(nullptr, &webView2RuntimeVersionRaw)) &&
			webView2RuntimeVersionRaw) {
			webView2RuntimeVersion = ToUtf8(webView2RuntimeVersionRaw);
			::CoTaskMemFree(webView2RuntimeVersionRaw);
		} else {
			app.log.Warn("WebView2 Runtime version unavailable; environment creation will report the failure to OSF Settings.");
		}
		if (!app.Send(osfui::browser::msg::ToJson(osfui::browser::msg::Hello{
				.protocolVersion = kBrowserHostProtocolVersion,
				.runtimeVersion = webView2RuntimeVersion,
                .backend = "webview2",
                .capabilities = kRequiredHostCapabilities,
			}))) {
			app.log.Error("hello could not be queued");
			return 7;
		}
		app.log.Info("hello queued (WebView2 Runtime " + webView2RuntimeVersion + ")");

		app.reader = std::thread([&app] { app.ReaderMain(); });
		int code = 10;
		try {
			code = app.Run();
		} catch (const winrt::hresult_error& e) {
			app.log.Error("unhandled browser-host failure: " + ToUtf8(e.message()));
		}
		// Run() normally stops both transport threads; this also covers the exception path.
		app.quit.store(true);
		::SetEvent(app.wakeEvent);
		app.StopTransport();
		return code;
	}
}
