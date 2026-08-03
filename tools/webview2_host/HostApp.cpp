#include "HostApp.h"

#include "EmbeddedScripts.h"

#include "core/Version.h"
#include "input/ScanCode.h"
#include "reporting/ReporterCore.h"
#include "Wv2BoundedQueue.h"
#include "Wv2BrokerLaunch.h"  // LaunchMethodName (logging only)
#include "Wv2LocalUri.h"
#include "Wv2Pipe.h"
#include "Wv2Protocol.h"
#include "Win32Util.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <vector>

#include <DispatcherQueue.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <shellapi.h>
#include <shlobj.h>
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
		using osfui::win32::ToUtf8;
		using osfui::win32::ToWide;

		// Physical identity of an accelerator key (DIK convention), matching
		// the game side's OverlayInputHook::MessageScanCode: compose from the
		// message's scan fields, fall back to the layout's VK->scan mapping
		// when a synthesized message carried none.
		std::uint32_t ComposeAcceleratorScan(std::uint32_t a_vk,
			std::uint32_t a_rawScan, bool a_extended)
		{
			const auto scan = OSFUI::ComposeScanCode(a_vk,
				static_cast<std::uint8_t>(a_rawScan & 0xFF), a_extended);
			if (scan != OSFUI::kInvalidScanCode) {
				return scan;
			}
			const UINT composite = ::MapVirtualKeyW(a_vk, MAPVK_VK_TO_VSC_EX);
			if (composite == 0) {
				return 0;
			}
			const UINT prefix = composite >> 8;
			const UINT base = composite & 0xFFu;
			return (prefix == 0xE0u || prefix == 0xE1u) ? (0x80u | base) : base;
		}

		using OSFUI::Reporting::DumpSafe;

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
				// Keep the previous session's log (mirrors the plugin's logRotate=1):
				// after a crash the file is all the forensics there is, and it must
				// survive the next launch or the report prompt has nothing to attach.
				if (std::filesystem::exists(a_path, ec)) {
					auto old = a_path;
					old.replace_extension(".old.log");
					std::filesystem::rename(a_path, old, ec);
				}
				file.open(a_path, std::ios::out | std::ios::trunc);
			}

			void Flush()
			{
				std::scoped_lock lock(mutex);
				if (file.is_open()) file.flush();
			}

			// level: 0 info, 1 warn, 2 error
			void Log(int a_level, const std::string& a_text)
			{
				{
					std::scoped_lock lock(mutex);
					if (file.is_open()) {
						// Date matches the plugin log's pattern so the two files
						// correlate at a glance across sessions.
						const auto now = std::chrono::system_clock::now();
						file << std::format("[{:%m-%d %H:%M:%S}] [{}] {}\n",
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
				if (auto* target = pipe.load(std::memory_order_acquire)) {
					target->WriteMessage(DumpSafe(json{
						{ "type", "log" }, { "level", a_level }, { "text", a_text } }));
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

		std::filesystem::path DocumentsFolder()
		{
			PWSTR raw = nullptr;
			std::filesystem::path folder;
			if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) {
				folder = raw;
			}
			if (raw) ::CoTaskMemFree(raw);
			return folder;
		}

		bool IsCrashLogCandidate(const std::filesystem::path& a_path)
		{
			auto extension = a_path.extension().wstring();
			std::ranges::transform(extension, extension.begin(),
				[](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
			return extension == L".log" || extension == L".txt";
		}

		struct CrashReportTarget
		{
			std::string id{ "osf-ui" };
			std::wstring repositoryLabel{ L"OSF UI" };
			std::optional<std::filesystem::path> pluginLog;
		};

		CrashReportTarget TargetForView(std::string_view a_viewId,
			const std::filesystem::path& a_hostLog)
		{
			if (a_viewId.starts_with("osf.animation/")) {
				return {
					.id = "osf-animation",
					.repositoryLabel = L"OSF Animation",
					.pluginLog = a_hostLog.parent_path() / "OSF Animation.log",
				};
			}
			return {};
		}

		std::optional<std::filesystem::path> FindSessionPluginLog(
			const std::optional<std::filesystem::path>& a_path,
			std::filesystem::file_time_type a_sessionStarted)
		{
			if (!a_path) return std::nullopt;
			std::error_code ec;
			if (!std::filesystem::is_regular_file(*a_path, ec) || ec) return std::nullopt;
			const auto size = std::filesystem::file_size(*a_path, ec);
			if (ec || size == 0) return std::nullopt;
			const auto modified = std::filesystem::last_write_time(*a_path, ec);
			return !ec && modified >= a_sessionStarted ? a_path : std::nullopt;
		}

		bool IsRecognizedCrashLog(const std::filesystem::path& a_path)
		{
			constexpr std::size_t kProbeBytes = 16 * 1024;
			std::ifstream file(a_path, std::ios::binary);
			if (!file) return false;
			std::string probe(kProbeBytes, '\0');
			file.read(probe.data(), static_cast<std::streamsize>(probe.size()));
			probe.resize(static_cast<std::size_t>(file.gcount()));
			std::ranges::transform(probe, probe.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			const bool hasException = probe.contains("unhandled exception");
			const bool supportedLogger = probe.contains("trainwreck") ||
				probe.contains("crashlogger") || probe.contains("crash logger");
			return hasException && supportedLogger;
		}

		std::optional<std::filesystem::path> FindSessionCrashLog(
			const std::filesystem::path& a_hostLog,
			std::filesystem::file_time_type a_sessionStarted)
		{
			if (a_hostLog.empty()) return std::nullopt;
			const auto directory = a_hostLog.parent_path().parent_path() / "Crashlogs";
			std::optional<std::filesystem::path> newest;
			std::filesystem::file_time_type newestTime{};
			std::error_code ec;
			for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end;
				it.increment(ec)) {
				if (!it->is_regular_file(ec) || ec || !IsCrashLogCandidate(it->path())) {
					ec.clear();
					continue;
				}
				const auto size = it->file_size(ec);
				if (ec) { ec.clear(); continue; }
				const auto modified = it->last_write_time(ec);
				if (ec) { ec.clear(); continue; }
				if (size != 0 && modified >= a_sessionStarted &&
					IsRecognizedCrashLog(it->path()) && (!newest || modified > newestTime)) {
					newest = it->path();
					newestTime = modified;
				}
			}
			return newest;
		}

		[[nodiscard]] std::string DescribeWindow(HWND a_window)
		{
			if (!a_window) return "none";
			wchar_t className[64]{};
			::GetClassNameW(a_window, className, static_cast<int>(std::size(className)));
			DWORD pid = 0;
			::GetWindowThreadProcessId(a_window, &pid);
			std::wstring exe = L"?";
			if (HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
				wchar_t path[MAX_PATH]{};
				DWORD length = static_cast<DWORD>(std::size(path));
				if (::QueryFullProcessImageNameW(process, 0, path, &length)) {
					exe = std::filesystem::path(path).filename().wstring();
				}
				::CloseHandle(process);
			}
			return std::format("hwnd=0x{:X} class='{}' pid={} exe='{}'",
				reinterpret_cast<std::uintptr_t>(a_window), ToUtf8(className), pid, ToUtf8(exe));
		}

		// The crash-report dialogs compete with whatever owns the desktop right
		// after a game exit — MO2's always-on-top lock overlay (the host itself
		// keeps the MO2 session locked while the prompt is up), crash loggers,
		// third-party overlays. A topmost window layered above the dialog
		// swallows every click while keyboard input still reaches it (field
		// report: "can't click the buttons, Enter works"). The watchdog names
		// the offending window in the host log and re-raises the dialog so
		// clicks land where the player sees them.
		int GuardedMessageBox(const std::wstring& a_text, const wchar_t* a_title,
			UINT a_flags, Logger& a_log)
		{
			std::atomic_bool done{ false };
			static constexpr std::uint64_t kAutoDismissMs = 60 * 1000;
			const auto started = ::GetTickCount64();
			const int timeoutChoice = (a_flags & MB_TYPEMASK) == MB_YESNO ? IDNO : IDOK;
			std::thread watchdog([&done, a_title, &a_log, started, timeoutChoice] {
				HWND lastCover = nullptr;
				HWND lastForeground = nullptr;
				bool timeoutLogged = false;
				std::uint64_t lastTimeoutPost = 0;
				while (!done.load()) {
					::Sleep(250);
					const HWND dialog = ::FindWindowW(L"#32770", a_title);
					if (!dialog) continue;
					const auto now = ::GetTickCount64();
					if (now - started >= kAutoDismissMs &&
						(lastTimeoutPost == 0 || now - lastTimeoutPost >= 1000)) {
						if (!timeoutLogged) {
							timeoutLogged = true;
							a_log.Warn(std::format(
								"dialog '{}' unanswered for 60s — choosing {} so the host can exit",
								ToUtf8(a_title), timeoutChoice == IDNO ? "No" : "OK"));
						}
						lastTimeoutPost = now;
						const HWND button = ::GetDlgItem(dialog, timeoutChoice);
						::PostMessageW(dialog, WM_COMMAND,
							MAKEWPARAM(timeoutChoice, BN_CLICKED),
							reinterpret_cast<LPARAM>(button));
						continue;
					}
					RECT rect{};
					if (!::GetWindowRect(dialog, &rect)) continue;
					const POINT centre{ (rect.left + rect.right) / 2,
						(rect.top + rect.bottom) / 2 };
					const HWND atCentre = ::WindowFromPoint(centre);
					const HWND cover = atCentre ? ::GetAncestor(atCentre, GA_ROOT) : nullptr;
					const HWND foreground = ::GetForegroundWindow();
					const bool covered = cover && cover != dialog;
					const bool defocused = foreground && foreground != dialog;
					if (covered && cover != lastCover) {
						lastCover = cover;
						a_log.Warn(std::format("dialog '{}' covered by {} — re-raising",
							ToUtf8(a_title), DescribeWindow(cover)));
					}
					if (defocused && foreground != lastForeground) {
						lastForeground = foreground;
						a_log.Warn(std::format("dialog '{}' lost foreground to {}",
							ToUtf8(a_title), DescribeWindow(foreground)));
					}
					if (covered) {
						::SetWindowPos(dialog, HWND_TOPMOST, 0, 0, 0, 0,
							SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
					}
					if (defocused) {
						::SetForegroundWindow(dialog);
					}
				}
			});
			const auto choice = ::MessageBoxW(nullptr, a_text.c_str(), a_title, a_flags);
			done.store(true);
			watchdog.join();
			return choice;
		}

		void PromptCrashReport(const HostOptions& a_options, DWORD a_gameExitCode, Logger& a_log,
			std::filesystem::file_time_type a_sessionStarted, std::string_view a_activeViewId)
		{
			if (a_options.reportEndpoint.empty()) return;
			const auto target = TargetForView(a_activeViewId, a_options.logFile);
			std::wstring disclosure =
				L"Starfield closed unexpectedly. OSF UI may not have caused the crash, "
				L"but its diagnostic logs could help find the problem.\n\n"
				L"Submit a bug report to the " + target.repositoryLabel +
				L" repository now? The recent OSF UI and WebView2-host logs";
			if (target.pluginLog) {
				disclosure += L", " + target.pluginLog->filename().wstring();
			}
			disclosure += L", plus "
				L"the newest Trainwreck or Crash Logger report created during this game session "
				L"if one is present";
			disclosure +=
				L", will be redacted locally, uploaded privately, and deleted after 30 days. "
				L"The report will be reviewed before any public GitHub issue is created.";
			const auto choice = GuardedMessageBox(disclosure,
				L"OSF UI - Starfield closed unexpectedly",
				MB_YESNO | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND, a_log);
			if (choice != IDYES) {
				a_log.Info("crash report declined by the player");
				return;
			}
			const auto crashLog = FindSessionCrashLog(a_options.logFile, a_sessionStarted);
			const auto pluginLog = FindSessionPluginLog(target.pluginLog, a_sessionStarted);
			if (pluginLog) {
				a_log.Info(std::format("consented crash report includes target log: {}",
					ToUtf8(pluginLog->filename().wstring())));
			}
			if (crashLog) {
				a_log.Info(std::format("consented crash report includes supported session log: {}",
					ToUtf8(crashLog->filename().wstring())));
			}
			a_log.Info("crash report consented; collecting bounded redacted logs");
			a_log.Flush();
			wchar_t executable[32768]{};
			const auto executableLength = ::GetModuleFileNameW(nullptr, executable,
				static_cast<DWORD>(std::size(executable)));
			const auto mirrorRoot = executableLength > 0 ?
				ToUtf8(std::filesystem::path(executable).parent_path().parent_path().wstring()) :
				std::string{};
			const auto pluginRoot = ToUtf8(a_options.reportPluginRoot.wstring());
			const auto documentsRoot = ToUtf8(DocumentsFolder().wstring());
			json logs = json::array();
			const std::array redactions{
				OSFUI::Reporting::Redaction{ pluginRoot, "<PluginDir>" },
				OSFUI::Reporting::Redaction{ documentsRoot, "<Documents>" },
				OSFUI::Reporting::Redaction{ mirrorRoot, "<HostMirror>" },
			};
			const auto addLog = [&logs, &redactions](const std::filesystem::path& path,
				std::string_view name, std::size_t maxBytes) {
				bool truncated = false;
				auto content = OSFUI::Reporting::ReadTail(path, maxBytes, truncated);
				if (content.empty()) return;
				content = OSFUI::Reporting::Redact(std::move(content), redactions);
				logs.push_back({
					{ "name", name }, { "content", std::move(content) }, { "truncated", truncated } });
			};
			addLog(a_options.logFile.parent_path() / "OSF UI.log", "OSF UI.log", 160 * 1024);
			addLog(a_options.logFile, "OSF UI.webview2-host.log", 128 * 1024);
			if (pluginLog) {
				addLog(*pluginLog, ToUtf8(pluginLog->filename().wstring()), 224 * 1024);
			}
			if (crashLog) {
				addLog(*crashLog, "Starfield crash log (Trainwreck or Crash Logger)", 256 * 1024);
			}
			const auto endpoint = ToUtf8(a_options.reportEndpoint);
			const auto payload = [&](std::string_view clientId, std::string_view token) {
				return json{
					{ "schemaVersion", 1 }, { "clientId", clientId },
					{ "installationToken", token }, { "kind", "crash" },
					{ "target", target.id },
					{ "title", "Starfield closed unexpectedly" },
					{ "description", crashLog ?
						"Starfield exited with a non-zero process status and produced a supported crash report." :
						"Starfield exited with a non-zero process status while OSF UI was active." },
					{ "reproduction", "Not provided; submitted from the post-crash consent prompt." },
					{ "pluginVersion", OSFUI::kPluginVersion },
					{ "diagnostics", { { "system", { { "gameExitCode", a_gameExitCode },
						{ "crashLogDetected", crashLog.has_value() },
						{ "crashLogFile", crashLog ? ToUtf8(crashLog->filename().wstring()) : "" },
						{ "activeView", std::string(a_activeViewId) },
						{ "targetPluginLogAttached", pluginLog.has_value() } } },
						{ "issues", json::array() } } },
					{ "logs", logs },
				};
			};
			const auto submission = OSFUI::Reporting::SubmitAuthenticated(endpoint,
				OSFUI::Reporting::ReporterFolder(DocumentsFolder()), payload,
				OSFUI::Reporting::PostJson);
			if (submission.errorCode == "registration-failed") {
				a_log.Error("crash report installation registration failed");
				GuardedMessageBox(
					L"The reporting service could not register this installation. Your logs remain "
					L"in the Starfield SFSE Logs folder.", L"OSF UI - Report failed",
					MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND, a_log);
				return;
			}
			const auto& reply = submission.body;
			const bool accepted = submission.errorCode.empty() &&
				submission.response.status >= 200 && submission.response.status < 300 &&
				reply.value("ok", false);
			if (accepted) {
				const auto reportId = reply.value("reportId", "");
				a_log.Info(std::format("crash report accepted (reference {})", reportId));
				const auto message = std::format(
					L"The diagnostic report was accepted for review.\n\nReference: {}", ToWide(reportId));
				GuardedMessageBox(message, L"OSF UI - Report submitted",
					MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND, a_log);
			} else {
				a_log.Error(std::format("crash report submission failed (HTTP {}, {})",
					submission.response.status,
					submission.errorCode.empty() ? "service-failed" : submission.errorCode));
				GuardedMessageBox(
					L"The report could not be submitted. Your logs remain in the Starfield "
					L"SFSE Logs folder and were not retained by OSF UI.",
					L"OSF UI - Report failed",
					MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND, a_log);
			}
		}

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

					L"OSF UI - WebView2 Runtime missing",
					MB_YESNO | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
				if (choice == IDYES) {
					::ShellExecuteW(nullptr, L"open", kRuntimeDownloadUrl,
						nullptr, nullptr, SW_SHOWNORMAL);
				}
			}).detach();
		}

		// Controller creation can fail after the runtime version probe and the
		// environment callback have both succeeded. That leaves no browser surface
		// in which to explain the problem, so use the same out-of-process native
		// prompt as the missing-runtime path.
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
			bool             gameExitedUnexpectedly{ false };
			DWORD            gameExitCode{ 0 };
			// Tick of the last player-initiated close the game-side WndProc hook
			// reported (playerCloseRequest); 0 = never. Written by the reader
			// thread, read on the STA thread when deciding whether a non-zero
			// game exit deserves the crash prompt.
			std::atomic<std::uint64_t> playerCloseRequestedAt{ 0 };
			std::uint64_t    gameExitedAt{ 0 };  // STA thread; tick when the exit was observed
			std::uint64_t    gameWindowMissingSince{ 0 };  // STA thread; 0 while HWND is valid
			std::filesystem::file_time_type sessionStarted{
				std::filesystem::file_time_type::clock::now() };
			std::string crashActiveViewId;

			static constexpr std::size_t kMaxCommands = 1024;
			static constexpr std::size_t kCommandsPerDrain = 128;
			BoundedQueue<json> commands{ kMaxCommands };
			std::atomic_bool pipeDead{ false };
			std::atomic_bool commandOverflow{ false };
			std::atomic_bool shutdownRequested{ false };
			std::uint64_t nextHeartbeatAt{ 0 };  // STA thread only

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
				ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> exceptionReceiver;
				bool controllerRequested{ false };
				bool securityReady{ false };
				bool hidden{ true };
				// A standard HTML control (select, datalist, date/color picker)
				// has asked Chromium to show native popup UI. That popup owns the
				// next physical click, so the session-wide host capture must stand
				// down until the page reports that the picker closed.
				bool nativePopupOpen{ false };
				// Warn-once latch for scripted (non-gesture) window.open attempts;
				// they are dropped, and one log line per view is enough evidence.
				bool nonGestureOpenWarned{ false };
				// Page -> host traffic is untrusted even when nativeBridge=false:
				// every document can call chrome.webview.postMessage directly.
				// Bound both individual messages and accepted rate before they
				// allocate pipe/game-side queue entries.
				std::uint64_t pageMessageWindowStarted{ 0 };
				std::uint32_t pageMessagesThisWindow{ 0 };
				bool pageMessageTooLargeWarned{ false };
				bool pageMessageFloodWarned{ false };
				// One-shot hidden paint requested by the game. Unlike leaving the
				// controller visible indefinitely, this primes Chromium without
				// running closed-view animations for the rest of the session.
				bool prewarm{ false };
				bool prewarmPending{ false };
				std::uint64_t prewarmDeadline{ 0 };
				// Explicit idle suspension is latched by the game. Attempts are async;
				// activityGeneration plus the host-unique attempt id prevent a late
				// callback from re-suspending an active or newly recreated view.
				bool suspendRequested{ false };
				bool suspendInFlight{ false };
				bool suspended{ false };
				bool suspendFailureLogged{ false };
				std::uint64_t suspendActivityGeneration{ 0 };
				std::uint64_t suspendAttemptId{ 0 };
				std::uint64_t nextSuspendAttemptMs{ 0 };
				bool renderStats{ false };
				std::uint64_t renderStatsLastPageLogMs{ 0 };
				// Manifest (authoring) height, set by `navigate`: the page lays out at
				// this height and ApplyScale derives the rasterization scale from it.
				std::uint32_t logicalHeight{ kDefaultLogicalHeight };
				// Manifest nativeBridge permission, set by `navigate`. False skips
				// the window.osfui shim injection entirely (security-model.md rule
				// 6); the game side independently drops any message from a
				// bridge-less view, so this is defence in depth, not the only gate.
				bool bridge{ true };
				// Deferred visibility: a reveal waits for the page's first painted
				// frame after Chromium resume, and hides wait for pending reveals.
				bool          revealPending{ false };
				bool          hideDeferred{ false };
				std::uint64_t revealDeadline{ 0 };
				std::string   revealToken;
				std::uint64_t pendingPresentationEpoch{ 0 };
				int  order{ 0 };
				bool domSeen{ false }, navigationSucceeded{ false };
				std::wstring currentUrl;
				std::optional<std::wstring> pendingNavigate;
				std::deque<std::string> queuedPostWeb;
			};
			std::vector<std::unique_ptr<View>> views;  // creation order (= z tie-break)
			View* active{ nullptr };  // mouse/focus/synthetic-key target
			bool  captureStarted{ false };
			std::uint64_t nextSuspendAttemptId{ 1 };

			// accel state pushed by the game (touched only on the STA thread).
			// Physical scan codes (DIK convention, input/ScanCode.h) since
			// protocol 6.
			std::uint32_t toggleScan{ 0x44 /*F10*/ }, captureUpScan{ 0 };
			bool          captured{ false }, captureArmed{ false };
			// Whether an input-capturing menu owns real OS focus. HUD-only views
			// leave this false so Starfield stays foreground. During a grant the
			// host HWND captures legacy mouse input; keyboard/IME route naturally
			// to Chromium, and the game polls XInput independently of its suspended
			// Windows.Gaming.Input stream.
			bool          focusGranted{ false };
			bool          rawMouseRegistered{ false };
			int           capturedMouseX{ 0 }, capturedMouseY{ 0 };
			std::unordered_set<UINT> handledKeys;
			// Warn-once dedupe for denied egress, per view and origin. Hostnames
			// are page-controlled (`fetch('https://' + Math.random() + '.x/')`),
			// so the set AND the log lines it admits must both be bounded, or a
			// hostile page grows the log, the pipe, and this process without
			// limit (docs/logging.md: "Nothing may log unboundedly"). Past the
			// cap one terminal line announces that further denials are silent.
			// Entries are dropped with their view.
			static constexpr std::size_t kMaxEgressWarnsPerView = 32;
			std::unordered_map<std::string, std::unordered_set<std::string>> egressWarned;
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
			std::uint64_t              frameSerial{ 0 };
			std::uint32_t              lastSlot{ 0 };
			std::mutex                 captureEpochMutex;
			std::atomic<std::uint64_t> presentationEpoch{ 0 };
			// Serials the game released without a GPU read (hidden overlay, stale
			// ring): it has no device to CPU-signal the consume fence, so it acks
			// over the pipe instead.
			std::atomic<std::uint64_t> ackedSerial{ 0 };
			std::uint64_t consumeWaitTimeouts{ 0 };
			double        produceMsTotal{ 0.0 };

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

			bool Send(const json& a_msg)
			{
				if (pipe.WriteMessage(DumpSafe(a_msg))) return true;
				pipeDead.store(true, std::memory_order_release);
				if (wakeEvent) ::SetEvent(wakeEvent);
				return false;
			}

			void ReaderMain()
			{
				std::string payload;
				while (pipe.ReadMessage(payload)) {
					json parsed = json::parse(payload, nullptr, false);
					if (parsed.is_discarded()) {
						log.Warn("dropping unparseable pipe message");
						continue;
					}
					const auto type = parsed.value("type", std::string{});
					if (type == "playerCloseRequest") {
						playerCloseRequestedAt.store(
							std::max<std::uint64_t>(::GetTickCount64(), 1));
						log.Info("game window received a player close request");
						continue;
					}
					if (type == "shutdown") {
						shutdownRequested.store(true, std::memory_order_release);
						quit.store(true, std::memory_order_release);
						log.Info("shutdown request received from the game");
						::SetEvent(wakeEvent);
						break;
					}

					const auto coalesceKey = CommandCoalesceKey(type,
						parsed.value("kind", std::string{}),
						parsed.value("view", std::string{}));
					const auto result =
						commands.Push(std::move(parsed), coalesceKey);
					if (result == decltype(commands)::PushResult::Full) {
						commandOverflow.store(true, std::memory_order_release);
						quit.store(true, std::memory_order_release);
						log.Error(std::format(
							"game command queue exceeded {} messages; closing the helper",
							kMaxCommands));
						::SetEvent(wakeEvent);
						break;
					}
					if (result == decltype(commands)::PushResult::Closed) break;
					::SetEvent(wakeEvent);
				}
				pipeDead.store(true, std::memory_order_release);
				::SetEvent(wakeEvent);
			}
#include "HostGraphics.inl"

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
			// put_IsVisible(FALSE), which stops presentation and lets Chromium
			// throttle rendering, so on unhide it needs a few frames before it paints.
			// A menu switch arrives as
			// hide-old + show-new in one policy batch, so applying it verbatim blanks
			// the output for those frames. Instead: resume Chromium at once but keep
			// the child visual hidden until the page confirms a painted frame
			// (double-rAF sentinel posted as a web message the host intercepts), and
			// hold the batch's hides until every pending reveal completes or times
			// out — the old content stays up and the switch is one composition change.

			static constexpr std::string_view kRevealSentinelPrefix = "__osfuiRevealReady:";
			static constexpr const wchar_t* kPrewarmSentinelScript =
				L"requestAnimationFrame(function(){requestAnimationFrame(function(){"
				L"setTimeout(function(){chrome.webview.postMessage('__osfuiPrewarmReady');},0);"
				L"});});";
			static constexpr std::string_view kPrewarmSentinel = "__osfuiPrewarmReady";
			static constexpr std::uint64_t kRevealTimeoutMs = 300;
			static constexpr std::uint64_t kSuspendRetryMs = 5000;

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

			void ResumeCore(View& a_view)
			{
				if (!a_view.webView) return;
				ComPtr<ICoreWebView2_3> webView3;
				if (FAILED(a_view.webView.As(&webView3)) || !webView3) return;
				const auto hr = webView3->Resume();
				if (FAILED(hr)) {
					log.Warn(std::format("view '{}': Resume failed (0x{:08X})",
						a_view.id, static_cast<unsigned>(hr)));
				} else {
					a_view.suspended = false;
				}
			}

			void NoteViewActivity(View& a_view, bool a_clearSuspendRequest)
			{
				const bool mayNeedResume = a_view.suspendRequested ||
					a_view.suspendInFlight || a_view.suspended;
				++a_view.suspendActivityGeneration;
				if (a_clearSuspendRequest) {
					a_view.suspendRequested = false;
					a_view.nextSuspendAttemptMs = 0;
				} else if (a_view.suspendRequested) {
					a_view.nextSuspendAttemptMs = ::GetTickCount64() + kSuspendRetryMs;
				}
				if (mayNeedResume) {
					// Explicit even though visibility/navigation can also auto-resume:
					// PostWebMessage has no such documented guarantee, and a suspend
					// attempt may still be completing asynchronously.
					ResumeCore(a_view);
				}
			}

			void BeginPrewarm(View& a_view)
			{
				if (!a_view.prewarm || !a_view.hidden) return;
				if (!a_view.prewarmPending) {
					NoteViewActivity(a_view, /*a_clearSuspendRequest=*/false);
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
				a_view.pendingPresentationEpoch = 0;
				a_view.revealPending = false;  // cancel an in-flight reveal
				a_view.revealToken.clear();
				a_view.prewarmPending = false;
				a_view.prewarmDeadline = 0;
				a_view.hideDeferred = true;    // applied at batch end / reveal end
				log.Info(std::format("view '{}': hide (deferred to batch end)", a_view.id));
			}

			void ShowView(View& a_view)
			{
				NoteViewActivity(a_view, /*a_clearSuspendRequest=*/true);
				if (!a_view.hidden) {
					a_view.hideDeferred = false;
					log.Info(std::format("view '{}': show — already visible (visual={})",
						a_view.id, a_view.visual && a_view.visual.IsVisible()));
					// Nothing changes on screen, so WGC will not capture; republish
					// the current (genuine, on-screen) pixels under the new epoch.
					if (PromotePresentation(a_view)) {
						RepublishLatest();
					}
					return;
				}
				a_view.hidden = false;
				a_view.hideDeferred = false;
				a_view.prewarmPending = false;
				a_view.prewarmDeadline = 0;
				// Visibility also auto-resumes, after the explicit Resume above; nothing
				// paints while a successful TrySuspend remains in force.
				if (a_view.controller) a_view.controller->put_IsVisible(TRUE);
				if (a_view.visual && a_view.visual.IsVisible()) {
					log.Info(std::format(
						"view '{}': show — hide was still deferred, never left the screen", a_view.id));
					// Same as above: the composition is unchanged, so only a
					// republish gets a frame with this epoch to the game.
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
					// No page to ask (still loading / no controller yet): show
					// directly; the runtime's overlay reveal gate covers boot.
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

			void TickSuspends()
			{
				const auto now = ::GetTickCount64();
				for (auto& owned : views) {
					auto& view = *owned;
					if (!view.suspendRequested || !view.hidden || view.hideDeferred ||
						view.revealPending || view.prewarmPending || view.pendingNavigate ||
						!view.domSeen || !view.controller || !view.webView ||
						view.suspendInFlight || now < view.nextSuspendAttemptMs) {
						continue;
					}
					BOOL controllerVisible = TRUE;
					if (FAILED(view.controller->get_IsVisible(&controllerVisible)) ||
						controllerVisible == TRUE) {
						continue;
					}
					ComPtr<ICoreWebView2_3> webView3;
					if (FAILED(view.webView.As(&webView3)) || !webView3) {
						if (!view.suspendFailureLogged) {
							view.suspendFailureLogged = true;
							log.Warn(std::format("view '{}': TrySuspend API unavailable", view.id));
						}
						view.nextSuspendAttemptMs = now + kSuspendRetryMs;
						continue;
					}
					BOOL actuallySuspended = FALSE;
					if (SUCCEEDED(webView3->get_IsSuspended(&actuallySuspended)) &&
						actuallySuspended == TRUE) {
						view.suspended = true;
						continue;
					}
					if (view.suspended) {
						// An API resumed it outside the explicit activity paths. Observe
						// reality and allow a short sync window before trying again.
						view.suspended = false;
						view.nextSuspendAttemptMs = now + kSuspendRetryMs;
						continue;
					}

					const auto id = view.id;
					const auto generation = view.suspendActivityGeneration;
					const auto attemptId = nextSuspendAttemptId++;
					view.suspendAttemptId = attemptId;
					view.suspendInFlight = true;
					view.nextSuspendAttemptMs = now + kSuspendRetryMs;
					const auto hr = webView3->TrySuspend(
						Callback<ICoreWebView2TrySuspendCompletedHandler>(
							[this, id, generation, attemptId](HRESULT a_error, BOOL a_success) -> HRESULT {
								auto* current = FindView(id);
								if (!current || current->suspendAttemptId != attemptId) return S_OK;
								current->suspendInFlight = false;
								const bool stale = current->suspendActivityGeneration != generation ||
									!current->suspendRequested || !current->hidden;
								if (stale) {
									if (SUCCEEDED(a_error) && a_success == TRUE) ResumeCore(*current);
									return S_OK;
								}
								if (SUCCEEDED(a_error) && a_success == TRUE) {
									current->suspended = true;
									current->suspendFailureLogged = false;
									log.Info(std::format("view '{}': idle suspend accepted", id));
								} else {
									current->suspended = false;
									current->nextSuspendAttemptMs =
										::GetTickCount64() + kSuspendRetryMs;
									if (!current->suspendFailureLogged) {
										current->suspendFailureLogged = true;
										log.Warn(std::format(
											"view '{}': TrySuspend declined (0x{:08X}); retrying while hidden",
											id, static_cast<unsigned>(a_error)));
									}
								}
								return S_OK;
							}).Get());
					if (FAILED(hr)) {
						view.suspendInFlight = false;
						if (!view.suspendFailureLogged) {
							view.suspendFailureLogged = true;
							log.Warn(std::format(
								"view '{}': TrySuspend call failed (0x{:08X}); retrying while hidden",
								view.id, static_cast<unsigned>(hr)));
						}
					}
				}
			}

			// STA thread only: iterates `views`, which the STA mutates unlocked
			// (push_back on createView, erase_if on destroyView). The capture
			// thread must read the cached atomic below instead — refreshed here,
			// and this runs at least once per STA message-loop iteration (the
			// MsgWaitForMultipleObjectsEx timeout selection), so the cache is at
			// most one iteration stale, which only affects stats-gap accounting.
			bool AnyVisibleRenderStats() const
			{
				bool any = false;
				for (const auto& view : views) {
					if (view->renderStats && !view->hidden && view->webView) {
						any = true;
						break;
					}
				}
				anyVisibleRenderStatsCache.store(any, std::memory_order_relaxed);
				return any;
			}
			// The one value OnFrameArrived (WGC free-threaded callback) may read
			// about views; ringMutex covers only the ring, never `views`.
			mutable std::atomic_bool anyVisibleRenderStatsCache{ false };

			void ApplyRenderStats(View& a_view)
			{
				if (!a_view.webView) return;
				if (a_view.suspendRequested || a_view.suspendInFlight || a_view.suspended) {
					NoteViewActivity(a_view, /*a_clearSuspendRequest=*/false);
				}
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
				const auto wide = ToWide(DumpSafe(sample));
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
				// Scrolling: Edge's "Windows scrolling personality" scales the wheel
				// distance to a PERCENTAGE of the hovered scroller's height, so a
				// short scroll container barely moves per notch while a tall one
				// jumps — wheel feel then depends on which element the cursor is
				// over. Disable it (under both names Chromium has shipped it —
				// unknown feature names are ignored, so stale entries are harmless
				// across runtime updates) while keeping the default smooth/impulse
				// scroll ANIMATION, which is distance-neutral and reads well in-game.
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

			void ReportControllerFailure(View& a_view, HRESULT a_hr,
				std::string_view a_description)
			{
				if (rendererFatal) return;
				rendererFatal = true;
				const auto code = static_cast<unsigned>(a_hr);
				log.Error(std::format("view '{}': {} (0x{:08X})",
					a_view.id, a_description, code));
				Send(json{
					{ "type", "fatal" },
					{ "stage", "composition-controller" },
					{ "view", a_view.id },
					{ "description", std::string(a_description) },
					{ "code", code },
				});
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
									 "untrusted content without the egress policy",
					a_view.id, a_description, code));
				Send(json{
					{ "type", "fatal" },
					{ "stage", "network-policy" },
					{ "view", a_view.id },
					{ "description", std::string(a_description) },
					{ "code", code },
				});
				quit.store(true);
				if (wakeEvent) ::SetEvent(wakeEvent);
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
				EnsureReparented();

				if (FAILED(a_view.compositionController.As(&a_view.controller)) ||
					FAILED(a_view.controller->get_CoreWebView2(&a_view.webView)) || !a_view.webView) {
					ReportControllerFailure(a_view, E_NOINTERFACE,
						"composition controller created but CoreWebView2 was unavailable");
					return S_OK;
				}
				// The Edge context menu is a real HWND-backed popup outside our
				// captured visual tree: it would draw over the game unclipped, and its
				// Back/Reload/Save/Inspect entries are meaningless for a mod view.
				// Suppressing it does NOT suppress the DOM `contextmenu` event, so
				// right-click still reaches page script.
				ComPtr<ICoreWebView2Settings> settings;
				if (SUCCEEDED(a_view.webView->get_Settings(&settings)) && settings) {
					settings->put_AreDefaultContextMenusEnabled(FALSE);
					settings->put_AreDevToolsEnabled(devMode ? TRUE : FALSE);
				} else {
					log.Warn(std::format("view '{}': settings unavailable — the browser "
						"context menu stays enabled", a_view.id));
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
				// Navigation and all authored page execution stay blocked until
				// the asynchronous document-created egress script is confirmed.
				// InstallNetworkGuard completes the rest of controller setup.
				result = InstallNetworkGuard(a_view);
				if (FAILED(result)) {
					ReportSecurityFailure(a_view, result,
						"network egress policy installation failed");
				}
				return S_OK;
			}

			HRESULT AddDocumentScript(View& a_view, const EmbeddedScript a_script,
				std::function<void(HRESULT)> a_completion)
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
				if (a_view.bridge) {
					InstallBridgeShim(a_view);
				} else {
					log.Info(std::format(
						"view '{}': nativeBridge=false — window.osfui not injected", a_view.id));
				}
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
					if (!StartCapture()) return;
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

			void InstallRenderStats(View& a_view)
			{
				const auto viewId = a_view.id;
				const auto hr = AddDocumentScript(a_view, EmbeddedScript::RenderStats,
					[this, viewId](const HRESULT a_scriptHr) {
						if (FAILED(a_scriptHr)) {
							log.Warn(std::format(
								"view '{}': render-stats shim install failed (0x{:08X})",
								viewId, static_cast<unsigned>(a_scriptHr)));
						}
					});
				if (FAILED(hr)) {
					log.Warn(std::format(
						"view '{}': render-stats shim registration failed (0x{:08X})",
						viewId, static_cast<unsigned>(hr)));
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

			// True only for the virtual-host origin the folder mapping serves.
			// The "/" (or end-of-string) right after the host is load-bearing:
			// it rejects https://osfui.local.evil.com/ and userinfo tricks like
			// https://osfui.local@evil.com/ without parsing the URI.
			// The decision itself lives in Wv2LocalUri.h so tests/native can
			// exercise it without a Windows host process.
			[[nodiscard]] bool IsLocalViewUri(std::wstring a_uri) const
			{
				return osfui::wv2::IsLocalViewUri(std::move(a_uri), virtualHost);
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
							auto& warned = egressWarned[view->id];
							if (warned.size() < kMaxEgressWarnsPerView &&
								warned.insert(ToUtf8(UriHost(uri))).second) {
								log.Warn(std::format(
									"view '{}': blocked network egress to {} (further "
									"denials for this origin are silent)",
									view->id, ToUtf8(uri)));
								if (warned.size() == kMaxEgressWarnsPerView) {
									log.Warn(std::format(
										"view '{}': {} distinct blocked origins — further "
										"egress denials for this view are silent",
										view->id, kMaxEgressWarnsPerView));
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
							COREWEBVIEW2_PHYSICAL_KEY_STATUS physical{};
							a_args->get_VirtualKey(&key);
							a_args->get_KeyEventKind(&kind);
							a_args->get_PhysicalKeyStatus(&physical);
							++accelEvents;
							const bool down =
								kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
								kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN;
							const auto scan = ComposeAcceleratorScan(key,
								physical.ScanCode, physical.IsExtendedKey != FALSE);
							// Only physical presses arrive here; synthetic gamepad keys
							// are DOM events and never reach this hook.
							// Synchronous stand-in for Runtime::OnNativeAcceleratorKey;
							// the game keeps this state fresh over the pipe (accelState).
							const bool frameworkOwned =
								captureArmed ||
								(captureUpScan != 0 && scan == captureUpScan) ||
								(toggleScan != 0 && scan == toggleScan) ||
								(devMode && key == VK_F12) ||
								(key == 0x1B && captured);
							const bool alreadyHandled = handledKeys.contains(key);
							// Opening a menu transfers keyboard focus from Starfield
							// to this WebView while the opening toggle can still be
							// physically down. Its first auto-repeat therefore arrives
							// here without a matching initial down in handledKeys.
							// WasKeyDown identifies both that cross-focus repeat and
							// ordinary repeats; neither is a second toggle intent.
							const bool duplicateDown = down &&
								(alreadyHandled || (frameworkOwned && physical.WasKeyDown));
							bool handled = duplicateDown;
							if (!handled && frameworkOwned) handled = true;
							if (!duplicateDown &&
								(frameworkOwned || (!down && alreadyHandled))) {
								Send(json{ { "type", "accelerator" },
									{ "vk", key }, { "scan", scan }, { "down", down } });
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
							constexpr std::size_t kMaxPageMessageChars = 64 * 1024;
							constexpr std::size_t kMaxPageMessageBytes = 64 * 1024;
							constexpr std::uint32_t kMaxPageMessagesPerSecond = 128;
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
								// Host-internal paint handshake; not forwarded.
								if (view->revealPending && text == view->revealToken) {
									CompleteReveal(*view, /*a_timedOut=*/false);
								}
								return S_OK;
							}
							if (text == kPrewarmSentinel) {
								// Host-internal one-shot warmup; not forwarded.
								CompletePrewarm(*view);
								return S_OK;
							}
							static constexpr std::string_view kNativePopupPrefix = "__osfuiNativePopup:";
							if (text.starts_with(kNativePopupPrefix)) {
								// Host-internal input handshake; authored controls
								// must be able to use Chromium's standard popup UI.
								view->nativePopupOpen =
									text.substr(kNativePopupPrefix.size()) == "1";
								if (active == view) ApplyMouseCapture();
								return S_OK;
							}
							static constexpr std::string_view kStatsPrefix = "__osfuiRenderStatsPage:";
							if (text.starts_with(kStatsPrefix)) {
								const auto sampleNow = ::GetTickCount64();
								if (view->renderStats &&
									(sampleNow - view->renderStatsLastPageLogMs >= 2000)) {
									const auto sample = json::parse(text.substr(kStatsPrefix.size()), nullptr, false);
									if (!sample.is_discarded()) {
										try {
											view->renderStatsLastPageLogMs = sampleNow;
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
							if (!view->bridge) return S_OK;
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
							// Only a real user gesture (a click on a link) may leave the
							// game. A scripted window.open never issues a network request,
							// so InstallNetworkGuard's default-deny cannot see it — without
							// this gate it is an egress channel around security-model.md
							// rule 2, carrying any payload in the query string.
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
							return S_OK;
						}).Get(), &token);
				ComPtr<ICoreWebView2_2> webView2;
				if (SUCCEEDED(a_view.webView.As(&webView2))) {
					webView2->add_DOMContentLoaded(
						Callback<ICoreWebView2DOMContentLoadedEventHandler>(
							[this, view](ICoreWebView2*, ICoreWebView2DOMContentLoadedEventArgs*) -> HRESULT {
								view->domSeen = true;
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
					// Uncaught exceptions are NOT console calls: a view whose boot code
					// throws logs nothing through consoleAPICalled, so the game side
					// used to see a perfectly healthy load event over a blank page.
					// Forward them through the same console channel, pre-shaped as a
					// console.error so the game-side parser needs no special case.
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
									try {
										const auto details =
											json::parse(raw).value("exceptionDetails", json::object());
										// `description` carries the stack; fall back to the
										// bare text ("Uncaught") plus location when a
										// non-Error value was thrown.
										const auto exception =
											details.value("exception", json::object());
										text = exception.value("description",
											details.value("text", std::string("uncaught exception")));
										const auto url = details.value("url", std::string{});
										if (!url.empty()) {
											text += std::format(" ({}:{}:{})", url,
												details.value("lineNumber", 0) + 1,
												details.value("columnNumber", 0) + 1);
										}
									} catch (const std::exception&) {
									}
									Send(json{ { "type", "console" }, { "view", view->id },
										{ "json", json{ { "type", "error" },
											{ "args", json::array({ json{ { "value",
												"uncaught: " + text } } }) } }
													  .dump() } });
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
					decltype(a_pool.TryGetNextFrame()) capturedFrame{ nullptr };
					std::uint64_t framePresentationEpoch = 0;
					{
						// Recreate+promotion takes the same lock. A callback
						// therefore either removes an old queued frame with the
						// old epoch, or observes the newly drained pool and epoch.
						std::scoped_lock epochLock(captureEpochMutex);
						framePresentationEpoch =
							presentationEpoch.load(std::memory_order_acquire);
						capturedFrame = a_pool.TryGetNextFrame();
					}
					if (!capturedFrame) return;
					const auto arrival = std::chrono::steady_clock::now();
					// Capture thread: never touch `views` here (STA mutates it
					// unlocked); the STA-refreshed cache answers this cheaply.
					if (anyVisibleRenderStatsCache.load(std::memory_order_relaxed) &&
						captureLastArrival.time_since_epoch().count() != 0) {
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
					captureArrivalCount.fetch_add(1, std::memory_order_relaxed);
					auto access = capturedFrame.Surface().as<
						::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
					ComPtr<ID3D11Texture2D> source;
					winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&source)));
					D3D11_TEXTURE2D_DESC desc{};
					source->GetDesc(&desc);
					// No warmup drop: a static page may paint fewer than 3 times in
					// total, so the first captured frame has to publish.
					PublishFrame(source.Get(), desc.Width, desc.Height,
						framePresentationEpoch);
				} catch (const winrt::hresult_error& a_error) {
					log.Warn(std::format("capture callback failed: {}", ToUtf8(a_error.message())));
				}
			}

			// Command handling; STA thread.

			void DrainQueuedViewWork(View& a_view)
			{
				if (!a_view.webView) return;
				if (a_view.pendingNavigate) {
					a_view.domSeen = a_view.navigationSucceeded = false;
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
				if (!a_view.queuedPostWeb.empty()) {
					NoteViewActivity(a_view, /*a_clearSuspendRequest=*/false);
				}
				for (auto& message : a_view.queuedPostWeb) {
					const auto wide = ToWide(message);
					a_view.webView->PostWebMessageAsString(wide.c_str());
				}
				a_view.queuedPostWeb.clear();
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
				std::scoped_lock epochLock(captureEpochMutex);
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
				// Raw input is the authoritative physical-wheel source while native
				// menu focus is live. A legacy WM_MOUSEWHEEL can still reach a
				// same-process widget on some WebView2 builds; consume that duplicate.
				if (rawMouseRegistered) return true;

				const auto delta = static_cast<SHORT>(HIWORD(a_wparam));
				if (delta == 0) return false;
				const POINT at = LiveWheelPoint();
				active->compositionController->SendMouseInput(
					COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL,
					static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(
						static_cast<UINT32>(LOWORD(a_wparam))),
					static_cast<UINT32>(delta), at);
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
						const auto scan = ComposeAcceleratorScan(vk,
							static_cast<std::uint32_t>((a_lparam >> 16) & 0xFF),
							(a_lparam & 0x01000000) != 0);
						// Same envelope as the accelerator path, so the game side
						// needs no new message type. Swallowed: mid-rebind the
						// press is a binding, not text for the page.
						self->Send(json{ { "type", "accelerator" },
							{ "vk", vk }, { "scan", scan }, { "down", true } });
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
				const bool captureForPage =
					focusGranted && (!active || !active->nativePopupOpen);
				if (captureForPage && hostWindow) {
					if (::GetCapture() != hostWindow) {
						::SetCapture(hostWindow);
					}
				} else {
					if (hostWindow && ::GetCapture() == hostWindow) {
						::ReleaseCapture();
					}
				}
			}

			void SetRawMouseInput(bool a_enabled)
			{
				if (a_enabled == rawMouseRegistered || !hostWindow) return;
				RAWINPUTDEVICE rawDevice{
					.usUsagePage = 0x01,
					.usUsage = 0x02,
					.dwFlags = static_cast<DWORD>(
						a_enabled ? RIDEV_INPUTSINK : RIDEV_REMOVE),
					.hwndTarget = a_enabled ? hostWindow : nullptr
				};
				if (!::RegisterRawInputDevices(&rawDevice, 1, sizeof(rawDevice))) {
					log.Warn(std::format("{} host raw mouse input failed ({}) — "
						"mouse wheel will use the legacy/game fallback",
						a_enabled ? "registering" : "removing", ::GetLastError()));
					return;
				}
				rawMouseRegistered = a_enabled;
			}

			/// View-space position of the REAL pointer, sampled at call time.
			/// The wheel must scroll whatever the page shows under the visible
			/// cursor, and `capturedMouseX/Y` cannot be trusted for that: it has
			/// two writers (the captured-HWND scaler and game-pipe moves carrying
			/// the runtime's virtual cursor), and whichever wrote last wins — a
			/// stale or drifted pipe move parks it at the clamp corner and every
			/// wheel notch then targets a dead pixel. Same scaling math as
			/// SendCapturedMouse; falls back to the cache only when the game
			/// window cannot be resolved (minimized/teardown).
			[[nodiscard]] POINT LiveWheelPoint() const
			{
				POINT point{};
				RECT  client{};
				if (gameTopLevel && ::GetCursorPos(&point) &&
					::ScreenToClient(gameTopLevel, &point) &&
					::GetClientRect(gameTopLevel, &client) &&
					client.right > client.left && client.bottom > client.top) {
					return POINT{
						std::clamp(::MulDiv(point.x - client.left,
							static_cast<int>(width), client.right - client.left),
							0, static_cast<int>(width) - 1),
						std::clamp(::MulDiv(point.y - client.top,
							static_cast<int>(height), client.bottom - client.top),
							0, static_cast<int>(height) - 1)
					};
				}
				return POINT{ capturedMouseX, capturedMouseY };
			}

			void SendRawMouseWheel(LPARAM a_lparam)
			{
				if (!focusGranted || !rawMouseRegistered || !active ||
					!active->compositionController) {
					return;
				}
				UINT size = 0;
				if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(a_lparam), RID_INPUT,
					nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 ||
					size == 0 || size > sizeof(RAWINPUT)) {
					return;
				}
				RAWINPUT raw{};
				if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(a_lparam), RID_INPUT,
					&raw, &size, sizeof(RAWINPUTHEADER)) != size ||
					raw.header.dwType != RIM_TYPEMOUSE ||
					(raw.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) == 0) {
					return;
				}
				const auto delta = static_cast<SHORT>(raw.data.mouse.usButtonData);
				if (delta == 0) return;
				const POINT at = LiveWheelPoint();
				active->compositionController->SendMouseInput(
					COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL,
					COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE,
					static_cast<UINT32>(delta), at);
			}

			[[nodiscard]] bool SendCapturedMouse(
				UINT a_msg, WPARAM a_wparam, LPARAM a_lparam)
			{
				if (!focusGranted || !active || !active->compositionController ||
					!hostWindow || !gameTopLevel) {
					return false;
				}
				if (a_msg == WM_MOUSEWHEEL && rawMouseRegistered) {
					return true;
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
				if (self && a_msg == WM_INPUT) {
					self->SendRawMouseWheel(a_lparam);
					// The original proc must still release the raw-input buffer.
				}
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
				const bool physicalWheel = kind == "physicalWheel";
				// Prefer direct host raw input and discard the later game-pipe
				// fallback so one physical notch scrolls exactly once.
				if (physicalWheel && rawMouseRegistered) return;
				int x = a_msg.value("x", 0);
				int y = a_msg.value("y", 0);
				// Right-stick scrolling still arrives over the pipe. Once the host
				// owns physical mouse capture, target the real pointer sampled at
				// send time rather than the game runtime's now-stale WM_INPUT
				// position (see LiveWheelPoint for why the cache cannot be used).
				if (focusGranted && (kind == "wheel" || physicalWheel)) {
					const POINT at = LiveWheelPoint();
					x = at.x;
					y = at.y;
				}
				if (kind == "move") {
					capturedMouseX = std::clamp(x, 0, static_cast<int>(width) - 1);
					capturedMouseY = std::clamp(y, 0, static_cast<int>(height) - 1);
				}
				COREWEBVIEW2_MOUSE_EVENT_KIND eventKind{};
				UINT32 data = 0;
				if (kind == "move") {
					eventKind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE;
				} else if (kind == "wheel" || physicalWheel) {
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

#include "HostCommands.inl"

			void DrainCommands()
			{
				decltype(commands)::Item item;
				std::size_t drained = 0;
				while (drained < kCommandsPerDrain && commands.TryPop(item)) {
					HandleCommand(item.value);
					++drained;
				}
				if (commands.Size() != 0) ::SetEvent(wakeEvent);
				// Hides deferred within this batch apply now, unless a reveal is
				// still waiting on its incoming view's first painted frame.
				if (!AnyRevealPending()) ApplyDeferredHides();
			}

			void CloseWebResources()
			{
				focusGranted = false;
				SetRawMouseInput(false);
				captureArmed = false;
				ReconcileInputWidgetSubclass();
				ApplyMouseCapture();
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
				active = nullptr;
				environment.Reset();
				captureStarted = false;
			}

			bool SendHeartbeatIfDue()
			{
				const auto now = ::GetTickCount64();
				if (nextHeartbeatAt != 0 && now < nextHeartbeatAt) return true;
				if (!Send(json{ { "type", "heartbeat" }, { "tick", now } })) {
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
						gameExitCode = code;
						gameExitedAt = ::GetTickCount64();
						// Some intentional game exits (notably the `qqq` console
						// command) use a non-zero process status. The helper exists to
						// catch the common crash-while-opening-OSF-UI failure, so require
						// evidence that the interactive overlay was active or mid-reveal.
						// HUD-only rendering and an inactive/closed overlay do not qualify.
						const bool uiCrashRelevant = focusGranted || captured || AnyRevealPending();
						gameExitedUnexpectedly = code != 0 && uiCrashRelevant;
						if (gameExitedUnexpectedly && active) {
							crashActiveViewId = active->id;
						}
						log.Info(std::format(
							"game process exited (code 0x{:08X}) — shutting down", gameExitCode));
						if (code != 0 && !uiCrashRelevant) {
							log.Info("non-zero game exit occurred while OSF UI was inactive — crash prompt suppressed");
						}
						return true;
					};
					while (!quit.load()) {
						if (!SendHeartbeatIfDue()) break;
						// Short timeout only while a reveal awaits its paint sentinel,
						// so the timeout fallback stays responsive.
						const DWORD wait = ::MsgWaitForMultipleObjectsEx(
							2, waits, AnyRevealPending() ? 50 : AnyVisibleRenderStats() ? 100 : 1000,
							QS_ALLINPUT, MWMO_INPUTAVAILABLE);
						if (wait == WAIT_OBJECT_0 + 1) {
							captureGameExit(0);
							break;
						}
						if (pipeDead.load()) {
							// A game crash can tear down the pipe a few milliseconds before
							// Windows signals the process handle. Resolve that race before
							// treating pipe loss as an ordinary renderer shutdown; otherwise
							// the exact "game crashed while opening UI" case skips its prompt.
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
							// The game may have recreated its top-level window
							// (display-device change, fullscreen transition) rather
							// than exited. Re-resolve by PID before concluding it is
							// gone; visible-only, so the game's hidden helper windows
							// (DXGI, IME) cannot satisfy the check forever.
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
						DrainCommands();
						TickReveals();
						TickSuspends();
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
				if (gameExitedUnexpectedly) {
					// A close the player asked for (taskbar "Close window",
					// title-bar X, Alt+F4, log-off) routinely tears Starfield
					// down through a path that dies with a non-zero status
					// (0xC0000005 observed) — not a crash worth prompting over.
					// Decided here, after the reader thread joined, so the
					// game's playerCloseRequest pipe message cannot race the
					// process-exit signal. The grace window keeps a real crash
					// long after an aborted close from being swallowed, while
					// still covering a slow teardown or a hung exit the player
					// finishes off via Task Manager.
					constexpr std::uint64_t kPlayerCloseGraceMs = 5 * 60 * 1000;
					const auto closeRequestedAt = playerCloseRequestedAt.load();
					if (closeRequestedAt != 0 &&
						gameExitedAt <= closeRequestedAt + kPlayerCloseGraceMs) {
						log.Info("non-zero game exit followed a player close request — "
								 "crash prompt suppressed");
					} else {
						PromptCrashReport(options, gameExitCode, log, sessionStarted,
							crashActiveViewId);
					}
				}
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

		// Production permits exactly one host per game process.
		const auto mutexName =
			std::format(L"Local\\osfui-wv2-host-{}", a_options.gamePid);
		const HANDLE instanceMutex = ::CreateMutexW(nullptr, TRUE, mutexName.c_str());
		if (!instanceMutex || ::GetLastError() == ERROR_ALREADY_EXISTS) {
			app.log.Error("another host instance is already running for this game pid");
			return 3;
		}

		// Pipe before OpenProcess: once log.pipe is set, every warning/error below
		// is forwarded into the game's own log, so a startup death is diagnosable
		// from "OSF UI.log" alone. The game tolerates log messages before hello.
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
		if (!app.Send(json{
				{ "type", "hello" },
				{ "protocolVersion", kProtocolVersion },
				{ "hostVersion", OSFUI::kPluginVersion },
				{ "runtimeVersion", runtime },
				{ "pid", ::GetCurrentProcessId() },
			})) {
			app.log.Error("hello write failed: " + app.pipe.LastErrorText());
			app.log.pipe.store(nullptr, std::memory_order_release);
			app.pipe.Close();
			::CloseHandle(app.gameProcess);
			::CloseHandle(instanceMutex);
			return 7;
		}
		app.log.Info("hello sent (WebView2 runtime " + runtime + ")");

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
			app.log.Error("unhandled host failure: " + ToUtf8(e.message()));
		} catch (const std::exception& e) {
			app.log.Error(std::string("unhandled host failure: ") + e.what());
		} catch (...) {
			app.log.Error("unhandled host failure: unknown exception");
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
