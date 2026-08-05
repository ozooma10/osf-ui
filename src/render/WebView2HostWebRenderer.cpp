#include "render/WebView2HostWebRenderer.h"

#include <atomic>
#include <deque>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "composite/EngineD3D12.h"
#include "core/Log.h"
#include "core/Version.h"
#include "runtime/DevViewFiles.h"
#include "input/OverlayInputHook.h"

#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>
#include <ShlObj.h>
#include <TlHelp32.h>
#include <d3d12.h>
#include <nlohmann/json.hpp>

#include "Wv2BoundedQueue.h"
#include "Wv2BrokerLaunch.h"
#include "Wv2Pipe.h"
#include "Wv2Protocol.h"
#include "Win32Util.h"

using nlohmann::json;

// The host posts this on an unsolicited focus grab outside an interactive
// menu session; both sides must agree on the value.
static_assert(OSFUI::OverlayInputHook::kRestoreGameFocusMessage ==
	osfui::wv2::kRestoreGameFocusMessage);

namespace OSFUI
{
	namespace
	{

		using osfui::win32::ToUtf8;
		using osfui::win32::ToWide;

		std::filesystem::path LocalOsfuiDir()
		{
			PWSTR value = nullptr;
			if (FAILED(::SHGetKnownFolderPath(
					FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &value))) {
				return std::filesystem::temp_directory_path() / "OSFUI";
			}
			std::filesystem::path out(value);
			::CoTaskMemFree(value);
			return out / "OSFUI";
		}

		// Next to "OSF UI.log": one folder to share covers plugin + host. The
		// SFSE log dir is a real (never VFS-virtualized) path, so the unhooked
		// host can write there.
		std::filesystem::path HostLogPath()
		{
			if (const auto dir = SFSE::log::log_directory()) {
				return *dir / "OSF UI.webview2-host.log";
			}
			return LocalOsfuiDir() / "webview2-host.log";
		}

		bool HasMarkOfTheWeb(const std::filesystem::path& a_file)
		{
			return ::GetFileAttributesW((a_file.native() + L":Zone.Identifier").c_str()) !=
			       INVALID_FILE_ATTRIBUTES;
		}

		bool HostProcessRunning()
		{
			const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
			if (snapshot == INVALID_HANDLE_VALUE) {
				return false;
			}
			PROCESSENTRY32W entry{ .dwSize = sizeof(PROCESSENTRY32W) };
			bool found = false;
			if (::Process32FirstW(snapshot, &entry)) {
				do {
					if (::_wcsicmp(entry.szExeFile, L"osfui_webview2_host.exe") == 0) {
						found = true;
						break;
					}
				} while (::Process32NextW(snapshot, &entry));
			}
			::CloseHandle(snapshot);
			return found;
		}

		// Last lines of the host's own log, for embedding into this log when the
		// host fails before/at the handshake — one shared file then tells the
		// whole story.
		std::vector<std::string> ReadLogTail(const std::filesystem::path& a_file,
			std::size_t a_maxLines)
		{
			std::ifstream stream(a_file, std::ios::binary);
			if (!stream) {
				return {};
			}
			constexpr std::streamoff kMaxBytes = 8192;
			stream.seekg(0, std::ios::end);
			const std::streamoff size = stream.tellg();
			stream.seekg(size > kMaxBytes ? size - kMaxBytes : 0, std::ios::beg);
			std::string chunk(static_cast<std::size_t>((std::min)(size, kMaxBytes)), '\0');
			stream.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
			chunk.resize(static_cast<std::size_t>(stream.gcount()));

			std::vector<std::string> lines;
			std::size_t start = 0;
			for (std::size_t i = 0; i <= chunk.size(); ++i) {
				if (i == chunk.size() || chunk[i] == '\n') {
					auto line = chunk.substr(start, i - start);
					while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
						line.pop_back();
					}
					if (!line.empty()) {
						lines.push_back(std::move(line));
					}
					start = i + 1;
				}
			}
			if (lines.size() > a_maxLines) {
				lines.erase(lines.begin(),
					lines.begin() + static_cast<std::ptrdiff_t>(lines.size() - a_maxLines));
			}
			return lines;
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

		// ---- Outbound host-message builders -------------------------------
		//
		// Each shape below is authored in two places: the connect-time snapshot
		// in the worker (holding stateMutex, walking `views`) and the matching
		// per-view setter (game thread, after the lock is released). One builder
		// per shape so the two cannot drift. Wire shapes are specified in
		// tools/webview2_shared/Wv2Protocol.h and parsed by a separate binary.
		//
		// PRIMITIVE / VIEW ARGUMENTS ONLY — never a `const ViewRec&`. The setter
		// send paths run OUTSIDE stateMutex by design and read their own
		// by-value parameters; `views` reallocates, so a ViewRec reference
		// dereferenced there would be a new data race. (ViewRec is not even
		// declared yet at this point in the file — keep it that way.)
		// string_view arguments are COPIED into the json before the builder
		// returns: never store one, never defer. The snapshot binds them to
		// ViewRec fields while it still holds stateMutex, so the copy happens
		// under the lock, on the worker thread, exactly as the inline code did.
		//
		// These builders only BUILD: send-gating (Send vs SendOrQueue vs the
		// raw pipe write) and every state side-effect stay in the callers.

		json ViewMsg(std::string_view a_type, std::string_view a_viewId)
		{
			return json{ { "type", std::string(a_type) },
				{ "view", std::string(a_viewId) } };
		}

		// NOTE: "navigate" keys the view id as "id", not "view" (wire protocol),
		// so it is deliberately not a ViewMsg.
		json NavigateMsg(std::string_view a_viewId, std::string_view a_entry,
			bool a_bridge, bool a_legacyApi, std::uint32_t a_logicalHeight)
		{
			return json{
				{ "type", "navigate" },
				{ "id", std::string(a_viewId) },
				{ "entry", std::string(a_entry) },
				{ "bridge", a_bridge },
				{ "legacyApi", a_legacyApi },
				{ "logicalHeight", a_logicalHeight } };
		}

		json PrewarmMsg(std::string_view a_viewId)
		{
			return ViewMsg("prewarm", a_viewId);
		}

		json SuspendMsg(std::string_view a_viewId)
		{
			return ViewMsg("suspendView", a_viewId);
		}

		json SetActiveMsg(std::string_view a_viewId)
		{
			return ViewMsg("setActive", a_viewId);
		}

		json SetHiddenMsg(std::string_view a_viewId, bool a_hidden,
			std::uint64_t a_presentationEpoch)
		{
			return json{ { "type", "setHidden" },
				{ "view", std::string(a_viewId) }, { "hidden", a_hidden },
				{ "presentationEpoch", a_presentationEpoch } };
		}

		json SetOrderMsg(std::string_view a_viewId, int a_order)
		{
			return json{ { "type", "setOrder" },
				{ "view", std::string(a_viewId) }, { "order", a_order } };
		}

		json FocusMsg(bool a_focused)
		{
			return json{ { "type", "focus" }, { "focused", a_focused } };
		}

		// Argument ORDER is the hazard here: captured then captureArmed. The
		// snapshot's locals are named accCaptured/accArmed while the wire key is
		// "captureArmed", so a swap would compile clean and change the wire.
		// Toggle and capture-up are physical SCAN codes (DIK convention) since
		// protocol 6 — the host matches framework-owned keys on them.
		json AccelStateMsg(std::uint32_t a_toggleScan,
			bool a_captured, bool a_captureArmed, std::uint32_t a_captureUpScan)
		{
			return json{
				{ "type", "accelState" },
				{ "toggleScan", a_toggleScan },
				{ "captured", a_captured }, { "captureArmed", a_captureArmed },
				{ "captureUpScan", a_captureUpScan } };
		}

		json FrameAckMsg(std::uint64_t a_serial)
		{
			return json{ { "type", "frameAck" }, { "serial", a_serial } };
		}

	}

	struct WebView2HostWebRenderer::Impl
	{
		struct Notify
		{
			enum class Kind { Web, Load, Fatal, Console, Ring, Log, Dead };
			Kind           kind{ Kind::Web };
			std::string    view;
			std::string    text, detail;
			bool           failed{};
			int            code{};
			std::uint32_t  unsignedCode{};
			std::uint64_t  id{};
			SharedRingDesc ring{};
		};

		RendererConfig        config;
		std::filesystem::path viewsRoot, mappedViewsRoot, userData;
        // Initial MO2 mirroring and later dev refreshes can run on different
        // threads; serialize the live real-path tree and its activation flag.
        std::mutex            viewsMirrorMutex;
        bool                  usesViewsMirror{ false };
		std::filesystem::path hostExeSource, hostExeMirror;
		std::filesystem::path hostLog;  // set in Initialize; read by worker + notify drain
		std::uint32_t adapterLuidLow{ 0 }, adapterLuidHigh{ 0 };
		bool          adapterLuidKnown{ false };

		WebMessageHandler       onWebMessage;
		LoadHandler             onLoad;
		FailureHandler          onFailure;
		CursorChangeHandler     onCursorChange;
		NativeAcceleratorHandler onAccelerator;
		SharedRingHandler       onSharedRing;
		HealthHandler           onHealth;
		// Game-thread only (Drain/setters).
		std::unordered_map<std::string, ConsoleHandler>    consoleHandlers;  // viewId -> cb

		// State the worker snapshots at connect time; later changes are sent
		// as diffs from the calling thread (WriteMessage is thread-safe).
		// Record order is creation order — the host's z tie-break.
		struct ViewRec
		{
			std::string id;
			std::string entry;
			bool        bridge{ false };
			bool        legacyApi{ false };
			bool        hidden{ true };
			bool        prewarm{ false };
			int         order{ 0 };
			// Manifest (authoring) height. The host divides output height by this
			// for the rasterization scale, so the page lays out at logical size
			// and CSS px scale up to output pixels.
			std::uint32_t logicalHeight{ kDefaultViewHeight };
		};
		std::mutex           stateMutex;
		std::vector<ViewRec> views;
		std::string          activeId;
		bool                 allHidden{ true };  // no visible view => Render() is never called
		// Every all-hidden -> visible transition starts a new presentation. Host
		// frames from before its reveal completes must not satisfy Runtime's
		// fresh-frame gate (closed capture frames are transparent).
		std::uint64_t        presentationEpoch{ 0 };
		std::uint32_t        width{ 1 }, height{ 1 };
		// accelState mirror (SetAcceleratorKeys diffs against this)
		std::uint32_t accToggle{ 0 }, accCaptureUp{ 0 };
		bool          accCaptured{ false }, accArmed{ false }, accSent{ false };

		enum class Lifecycle : std::uint8_t
		{
			Stopped,
			Starting,
			Running,
			Stopping,
			Failed
		};

		osfui::wv2::Pipe pipe;
		std::thread      worker;
		std::thread      writer;
		std::atomic<Lifecycle> lifecycle{ Lifecycle::Stopped };
		std::atomic_bool stopRequested{ false };
		std::atomic_bool connected{ false }, dead{ false };
		bool             deadLogged{ false };

		struct HostSession
		{
			DWORD  pid{ 0 };
			HANDLE process{ nullptr };
			HWND   topLevel{ nullptr };
		};
		std::mutex sessionMutex;
		HostSession session;

		// Focus watchdog (game thread only — SetNativeFocus and Update
		// both run there). Interactive menus grant real Win32 focus to a
		// cross-process Chromium child; HUD-only/closed states keep Starfield
		// focused. The revoke-time restore (posted kRestoreGameFocusMessage
		// -> SetFocus) races Chromium's asynchronous MoveFocus: an in-flight
		// focus grab can land after the restore and strand focus in the child,
		// leaving the game with no keyboard, no raw mouse input (WM_INPUT is
		// focus-gated) — and no gamepad, WGI being focus-gated too. Update()
		// detects that and re-asserts.
		std::atomic_bool focusRequested{ false };  // last SetNativeFocus argument
		double focusCheckAccum{ 0.0 };
		bool   focusFixWarned{ false };  // one WARN per strand episode

		// System Health reporting (protocol 1.4). The ring announcement is
		// parsed on the reader thread, but onHealth is a game-thread contract —
		// so the announced slot count is stashed here and Update() reports the
		// edge. Zero means "no announcement seen since the last report".
		std::atomic<std::uint32_t> ringSlotsAnnounced{ 0 };
		std::uint32_t              ringSlotsReported{ 0 };  // game thread only

		// Fire one health edge at the runtime. Game thread only.
		void ReportHealth(std::string_view a_code, bool a_active, std::string_view a_detail = {})
		{
			if (onHealth) {
				onHealth(HealthEvent{ a_code, a_active, a_detail });
			}
		}

		std::mutex         notifyMutex;
		std::deque<Notify> notifications;
		// Page traffic and forwarded logs can arrive while the game-thread drain is
		// paused. Lifecycle notifications have natural rates; untrusted Web/Console
		// and page-provokable Log entries do not, so cap them independently.
		static constexpr std::size_t kMaxPendingWeb = 64;
		static constexpr std::size_t kMaxPendingConsole = 64;
		static constexpr std::size_t kMaxPendingLogs = 256;
		std::size_t pendingWebCount{ 0 };      // all guarded by notifyMutex
		std::size_t pendingConsoleCount{ 0 };
		std::size_t pendingLogCount{ 0 };
		std::size_t droppedWebCount{ 0 };
		std::size_t droppedConsoleCount{ 0 };
		std::size_t droppedLogCount{ 0 };

		// Latest shared-ring frame (reader thread writes, game thread reads).
		std::mutex    frameMutex;
		bool          haveFrame{ false };
		std::uint32_t frameSlot{ 0 };
		std::uint64_t frameSerial{ 0 };
		std::uint32_t frameWidth{ 0 }, frameHeight{ 0 };
		std::uint64_t sharedRingGeneration{ 0 };
		std::uint64_t submittedSerial{ 0 };
		std::uint32_t ringWidth{ 0 }, ringHeight{ 0 };
		std::uint64_t ringGeneration{ 0 };       // reader-side counter
		std::uint64_t announcedGeneration{ 0 };  // dispatched to the compositor


		void Push(Notify a_value)
		{
			std::scoped_lock lock(notifyMutex);
			if (a_value.kind == Notify::Kind::Web) {
				if (pendingWebCount >= kMaxPendingWeb) {
					++droppedWebCount;
					return;
				}
				++pendingWebCount;
			} else if (a_value.kind == Notify::Kind::Console) {
				if (pendingConsoleCount >= kMaxPendingConsole) {
					++droppedConsoleCount;
					return;
				}
				++pendingConsoleCount;
			} else if (a_value.kind == Notify::Kind::Log) {
				if (pendingLogCount >= kMaxPendingLogs) {
					++droppedLogCount;
					return;
				}
				++pendingLogCount;
			}
			notifications.push_back(std::move(a_value));
		}

		static constexpr std::size_t kMaxOutbound = 512;
		osfui::wv2::BoundedQueue<std::string> outbound{ kMaxOutbound };
		std::atomic<std::uint64_t> nextOutboundSequence{ 1 };
		std::mutex writerGateMutex;
		std::condition_variable writerGate;
		bool writerReady{ false };
		std::mutex writtenMutex;
		std::condition_variable written;
		std::uint64_t lastWrittenSequence{ 0 };
		bool writerFailed{ false };
		std::atomic_bool outboundOverflowed{ false };

		[[nodiscard]] HostSession SessionSnapshot()
		{
			std::scoped_lock lock(sessionMutex);
			return session;
		}

		void SetTopLevel(HWND a_topLevel)
		{
			std::scoped_lock lock(sessionMutex);
			session.topLevel = a_topLevel;
		}

		void SetHostProcess(DWORD a_pid, HANDLE a_process)
		{
			std::scoped_lock lock(sessionMutex);
			session.pid = a_pid;
			session.process = a_process;
		}

		HANDLE TakeHostProcess()
		{
			std::scoped_lock lock(sessionMutex);
			session.pid = 0;
			session.topLevel = nullptr;
			return std::exchange(session.process, nullptr);
		}

		void SignalDead(std::string_view a_reason)
		{
			connected.store(false, std::memory_order_release);
			lifecycle.store(Lifecycle::Failed, std::memory_order_release);
			if (!stopRequested.load(std::memory_order_acquire) &&
				!dead.exchange(true, std::memory_order_acq_rel)) {
				REX::ERROR("WebView2HostWebRenderer: {}", a_reason);
				Push(Notify{ .kind = Notify::Kind::Dead });
			}
			outbound.Close();
			pipe.Close();
		}

		std::string CoalesceKey(const json& a_msg)
		{
			const auto type = a_msg.value("type", std::string{});
			return osfui::wv2::CommandCoalesceKey(type,
				a_msg.value("kind", std::string{}),
				a_msg.value("view", std::string{}));
		}

		bool Enqueue(const json& a_msg, bool a_queueBeforeConnect)
		{
			std::scoped_lock gateLock(writerGateMutex);
			if (!a_queueBeforeConnect &&
				!connected.load(std::memory_order_acquire)) {
				return false;
			}
			const auto result = outbound.Push(a_msg.dump(), CoalesceKey(a_msg),
				nextOutboundSequence.fetch_add(1, std::memory_order_relaxed));
			if (result == decltype(outbound)::PushResult::Full) {
				if (!outboundOverflowed.exchange(true)) {
					SignalDead(std::format(
						"outbound command queue exceeded {} messages", kMaxOutbound));
				}
				return false;
			}
			return result != decltype(outbound)::PushResult::Closed;
		}

		void Send(const json& a_msg) { Enqueue(a_msg, false); }

		// Bridge events cannot be reconstructed from state. Keep them bounded
		// before the lazy helper starts, then place the connection snapshot ahead
		// of them atomically so no caller can overtake initialization.
		void SendOrQueue(const json& a_msg) { Enqueue(a_msg, true); }

		bool PublishConnected(std::vector<osfui::wv2::BoundedQueue<std::string>::Item> a_bootstrap)
		{
			{
				// Publication and shutdown's connected exchange share this lock:
				// Stop cannot miss a connection that races its startup.
				std::scoped_lock lock(writerGateMutex);
				if (stopRequested.load(std::memory_order_acquire) ||
					!outbound.Prepend(std::move(a_bootstrap))) {
					return false;
				}
				connected.store(true, std::memory_order_release);
				lifecycle.store(Lifecycle::Running, std::memory_order_release);
				writerReady = true;
			}
			writerGate.notify_all();
			return true;
		}

		void WriterMain()
		{
			bool ready = false;
			{
				std::unique_lock lock(writerGateMutex);
				writerGate.wait(lock, [this] {
					return writerReady || stopRequested.load(std::memory_order_acquire);
				});
				ready = writerReady;
			}
			if (!ready) return;

			decltype(outbound)::Item item;
			while (outbound.WaitPop(item)) {
				if (!pipe.WriteMessage(item.value)) {
					{
						std::scoped_lock lock(writtenMutex);
						writerFailed = true;
					}
					written.notify_all();
					if (!stopRequested.load(std::memory_order_acquire)) {
						SignalDead("outbound pipe writer failed: " + pipe.LastErrorText());
					}
					return;
				}
				{
					std::scoped_lock lock(writtenMutex);
					lastWrittenSequence =
						std::max(lastWrittenSequence, item.sequence);
				}
				written.notify_all();
			}
		}

		void WriterEntry() noexcept
		{
			try {
				WriterMain();
			} catch (const std::exception& e) {
				SignalDead(std::string("outbound writer threw: ") + e.what());
			} catch (...) {
				SignalDead("outbound writer threw an unknown exception");
			}
		}
		// stateMutex must be held.
		ViewRec* FindView(std::string_view a_id)
		{
			for (auto& view : views) {
				if (view.id == a_id) return &view;
			}
			return nullptr;
		}

		// stateMutex must be held.
		void RecomputeAllHidden()
		{
			allHidden = true;
			for (const auto& view : views) {
				if (!view.hidden) {
					allHidden = false;
					return;
				}
			}
		}

		// Startup (worker thread)

		// Mod Organizer 2 presents the mod folder only inside USVFS-hooked
		// processes, and the host and its browser children are unhooked: both
		// the views tree and the host exe must live at real paths before
		// anything outside the game can use them.
		void ResolveMappedViewsRoot()
		{
            std::scoped_lock mirrorLock(viewsMirrorMutex);
			mappedViewsRoot = viewsRoot;
            usesViewsMirror = false;
			if (!::GetModuleHandleW(L"usvfs_x64.dll")) return;
			std::error_code ec;
			// A fresh path per game process is deliberate. Reusing the stable
			// `views-mirror` folder allowed a stale browser/helper process (or a
			// failed recursive delete) to leave old shared-kit files in place.
			// The view bundles could then be current while shared/osfui.js was
			// old enough to lack APIs they call, making every native request a
			// silent no-op. Separate renderer instances still need distinct
			// names within the same game process.
			auto mirrorName = std::format("views-mirror-{}", ::GetCurrentProcessId());
			const auto mirror = LocalOsfuiDir() / mirrorName;
			std::filesystem::remove_all(mirror, ec);
			if (ec) {
				REX::WARN("WebView2HostWebRenderer: could not clear per-run views mirror '{}' "
						  "({}); the browser host will not start with potentially stale files",
					ToUtf8(mirror.native()), ec.message());
				return;
			}
			ec.clear();
			std::filesystem::create_directories(mirror.parent_path(), ec);
			ec.clear();
			std::filesystem::copy(viewsRoot, mirror,
				std::filesystem::copy_options::recursive |
					std::filesystem::copy_options::overwrite_existing, ec);
			if (ec) {
				REX::WARN("WebView2HostWebRenderer: views mirror copy failed ({}); "
						  "the browser may not resolve the direct path", ec.message());
				return;
			}
			mappedViewsRoot = mirror;
            usesViewsMirror = true;
			REX::INFO("WebView2HostWebRenderer: USVFS detected — views mirrored to {}",
				ToUtf8(mirror.native()));
		}

        // The unhooked browser cannot see MO2's VFS. Refresh exactly one live
        // real-path view on the dev worker before Runtime navigates it.
        bool RefreshViewFiles(std::string_view a_viewId)
        {
            if (!config.devMode) return true;
            std::scoped_lock mirrorLock(viewsMirrorMutex);
            if (!usesViewsMirror) return true;

            // Mirror the whole mod folder. The view folder alone holds only the
            // entry HTML: its hashed bundles are emitted to the sibling
            // views/<modId>/assets/ and reached through "../assets/...", so a
            // view-scoped sync copies a fresh index.html over a stale assets
            // directory and the reload 404s on a chunk that was never mirrored.
            const auto modFolder = std::filesystem::path(DevViewFiles::ModFolder(a_viewId));
            const auto source = viewsRoot / modFolder;
            const auto destination = mappedViewsRoot / modFolder;
            std::string error;
            if (!DevViewFiles::SyncTree(source, destination, error)) {
                REX::WARN("WebView2HostWebRenderer: dev reload could not mirror '{}' ({})",
                    a_viewId, error);
                return false;
            }
            return true;
        }

		// The host exe ships inside the mod folder (VFS-only under MO2) but is
		// launched by Explorer/the task scheduler, which cannot see the VFS.
		// Mirror it to a real, version-stamped path first.
		bool MirrorHostExe()
		{
			const auto mirrorDir = LocalOsfuiDir() / "bin" / kPluginVersion;
			hostExeMirror = mirrorDir / "osfui_webview2_host.exe";
			std::error_code ec;
			std::filesystem::create_directories(mirrorDir, ec);
			ec.clear();
			const auto sourceSize = std::filesystem::file_size(hostExeSource, ec);
			if (ec) {
				REX::ERROR("WebView2HostWebRenderer: host exe missing at {} ({})",
					hostExeSource.string(), ec.message());
				return false;
			}
			ec.clear();
			const auto mirrorSize = std::filesystem::file_size(hostExeMirror, ec);
			const bool haveMirror = !ec;
			const bool sameSize = haveMirror && mirrorSize == sourceSize;
			const bool sameTime = haveMirror &&
				std::filesystem::last_write_time(hostExeSource, ec) <=
					std::filesystem::last_write_time(hostExeMirror, ec);
			if (!(sameSize && sameTime)) {
				ec.clear();
				std::filesystem::copy_file(hostExeSource, hostExeMirror,
					std::filesystem::copy_options::overwrite_existing, ec);
				if (ec) {
					if (sameSize) {
						// In use by a previous session's host but content
						// matches the shipped binary — reuse it.
						REX::WARN("WebView2HostWebRenderer: host exe mirror busy; "
								  "reusing existing copy ({})", ec.message());
					} else {
						REX::ERROR("WebView2HostWebRenderer: host exe mirror copy "
								   "failed ({})", ec.message());
						return false;
					}
				}
			}
			// CopyFileW carries the Zone.Identifier stream from a downloaded
			// zip onto the mirror, and Explorer launching a Mark-of-the-Web exe
			// can hit a SmartScreen block nobody sees behind a fullscreen game.
			if (HasMarkOfTheWeb(hostExeMirror)) {
				if (::DeleteFileW((hostExeMirror.native() + L":Zone.Identifier").c_str())) {
					REX::INFO("WebView2HostWebRenderer: stripped Mark-of-the-Web from the "
							  "host exe mirror");
				} else {
					REX::WARN("WebView2HostWebRenderer: host exe mirror carries "
							  "Mark-of-the-Web and stripping it failed ({}) — SmartScreen "
							  "may silently block the host launch", ::GetLastError());
				}
			}
			return true;
		}

		bool Start()
		{
			auto expected = Lifecycle::Stopped;
			if (!lifecycle.compare_exchange_strong(expected, Lifecycle::Starting,
					std::memory_order_acq_rel)) {
				return expected == Lifecycle::Starting || expected == Lifecycle::Running;
			}

			// The host must create its D3D11 capture textures on the same adapter
			// as Starfield's D3D12 device.
			if (!adapterLuidKnown) {
				auto engine = LocateEngineD3D12();
				if (!engine) {
					lifecycle.store(Lifecycle::Stopped, std::memory_order_release);
					return false;
				}
				const auto luid = engine.device->GetAdapterLuid();
				engine.directQueue->Release();
				engine.device->Release();
				{
					std::scoped_lock lock(stateMutex);
					adapterLuidLow = luid.LowPart;
					adapterLuidHigh = static_cast<std::uint32_t>(luid.HighPart);
					adapterLuidKnown = true;
				}
				REX::DEBUG("WebView2HostWebRenderer: game adapter LUID 0x{:08X}:0x{:08X}",
					adapterLuidHigh, adapterLuidLow);
			}

			stopRequested.store(false, std::memory_order_release);
			connected.store(false, std::memory_order_release);
			outboundOverflowed.store(false, std::memory_order_release);
			{
				std::scoped_lock lock(writerGateMutex);
				writerReady = false;
			}
			{
				std::scoped_lock lock(writtenMutex);
				lastWrittenSequence = 0;
				writerFailed = false;
			}
			pipe.PrepareForOpen();
			try {
				writer = std::thread([this] { WriterEntry(); });
				worker = std::thread([this] { WorkerEntry(); });
			} catch (const std::exception& e) {
				REX::ERROR("WebView2HostWebRenderer: could not create transport thread: {}",
					e.what());
				stopRequested.store(true, std::memory_order_release);
				outbound.Close();
				writerGate.notify_all();
				pipe.Close();
				if (writer.joinable()) writer.join();
				lifecycle.store(Lifecycle::Failed, std::memory_order_release);
				if (!dead.exchange(true)) Push(Notify{ .kind = Notify::Kind::Dead });
				return false;
			}
			REX::DEBUG("WebView2HostWebRenderer: starting host transport threads");
			return true;
		}
		// Worker thread, after the host failed to launch/handshake: narrow "it
		// never connected" down to which stage died, using only what this
		// process can see, and embed the host's own log tail so one shared
		// "OSF UI.log" carries the whole story.
		void LogHostStartFailureDiagnostics(std::filesystem::file_time_type a_launchTime)
		{
			std::error_code ec;
			const auto exeSize = std::filesystem::file_size(hostExeMirror, ec);
			if (ec) {
				REX::ERROR("HostDiag: host exe mirror is GONE from {} ({}) — an "
						   "antivirus likely quarantined it; restore/exclude it and retry",
					hostExeMirror.string(), ec.message());
			} else if (HasMarkOfTheWeb(hostExeMirror)) {
				REX::ERROR("HostDiag: host exe mirror still carries Mark-of-the-Web — "
						   "SmartScreen has likely blocked the launch silently; unblock {} "
						   "(file Properties -> Unblock)", hostExeMirror.string());
			} else {
				REX::INFO("HostDiag: host exe mirror present ({} bytes, no Mark-of-the-Web)",
					exeSize);
			}

			if (osfui::win32::IsProcessElevated()) {
				REX::ERROR("HostDiag: the game runs elevated (as administrator) — a "
						   "brokered host launches unelevated and cannot open the game "
						   "process; run the game/MO2 without administrator rights");
			}

			REX::INFO("HostDiag: a process named osfui_webview2_host.exe {} running",
				HostProcessRunning() ? "IS still" : "is NOT");

			ec.clear();
			const auto logTime = std::filesystem::last_write_time(hostLog, ec);
			if (ec) {
				REX::ERROR("HostDiag: host log {} does not exist — the host process never "
						   "started (SmartScreen/antivirus block, or the exe failed to run)",
					hostLog.string());
				return;
			}
			if (logTime < a_launchTime) {
				REX::ERROR("HostDiag: host log {} is STALE (predates this launch) — the "
						   "host process never started this session (SmartScreen/antivirus "
						   "block, or the exe failed to run)", hostLog.string());
				return;
			}
			const auto tail = ReadLogTail(hostLog, 20);
			REX::INFO("HostDiag: host log tail ({} line(s) from {}):",
				tail.size(), hostLog.string());
			for (const auto& line : tail) {
				REX::INFO("HostDiag: | {}", line);
			}
		}

		void WorkerEntry() noexcept
		{
			try {
				WorkerMain();
			} catch (const std::exception& e) {
				SignalDead(std::string("connection worker threw: ") + e.what());
			} catch (...) {
				SignalDead("connection worker threw an unknown exception");
			}
		}

		void WorkerMain()
		{
			ResolveMappedViewsRoot();
			if (!MirrorHostExe()) {
				SignalDead("host executable preparation failed");
				return;
			}

			const HWND gameTopLevel = FindTopLevelWindow();
			SetTopLevel(gameTopLevel);

			auto pipeSeed = ::GetTickCount64() ^
				(static_cast<std::uint64_t>(::GetCurrentProcessId()) << 17);
			std::mt19937_64 rng(pipeSeed);
			const auto nonce = static_cast<std::uint32_t>(rng());
			const auto pipeName = std::format(L"{}{}-{:08x}",
				osfui::wv2::kPipePrefix, ::GetCurrentProcessId(), nonce);
			auto args = std::format(L"--pipe={} --game-pid={} --log=\"{}\"",
				pipeName, ::GetCurrentProcessId(), hostLog.native());
			if (!config.reportEndpoint.empty()) {
				args += std::format(L" --report-endpoint=\"{}\"", ToWide(config.reportEndpoint));
				if (!config.reportPluginRoot.empty()) {
					args += std::format(L" --report-plugin-root=\"{}\"",
						config.reportPluginRoot.native());
				}
			}

			// Claim the first server instance before launching the helper. This
			// removes the name-squatting window between launch and CreateNamedPipe.
			if (!pipe.CreateServer(pipeName)) {
				SignalDead("could not create the private host pipe: " + pipe.LastErrorText());
				return;
			}
			if (stopRequested.load(std::memory_order_acquire)) return;

			const bool usvfs = ::GetModuleHandleW(L"usvfs_x64.dll") != nullptr;
			const bool elevated = osfui::win32::IsProcessElevated();
			if (elevated && usvfs) {
				REX::WARN("WebView2HostWebRenderer: the game is running elevated under "
						  "MO2; using the elevated broker fallback");
			}
			const auto launchTime = std::filesystem::file_time_type::clock::now();
			const auto launch = osfui::wv2::LaunchDetached(
				hostExeMirror.native(), args, /*a_preferBroker=*/usvfs);
			if (!launch.ok) {
				REX::ERROR("WebView2HostWebRenderer: host launch failed [{}]", launch.detail);
				SignalDead("host launch failed");
				return;
			}
			REX::INFO("WebView2HostWebRenderer: host launched via {} (usvfs={}, elevated={}){}",
				osfui::wv2::LaunchMethodName(launch.method), usvfs, elevated,
				launch.detail.empty() ? "" : " detail=[" + launch.detail + "]");

			if (!pipe.WaitForClient(20000)) {
				REX::ERROR("WebView2HostWebRenderer: host never connected: {} "
						   "(host log: {})", pipe.LastErrorText(), hostLog.string());
				LogHostStartFailureDiagnostics(launchTime);
				SignalDead("host did not connect");
				return;
			}
			const auto peerPid = pipe.ClientProcessId();
			if (!peerPid) {
				SignalDead("could not identify the connected helper: " + pipe.LastErrorText());
				return;
			}

			// Startup diagnostics may precede hello, but the entire handshake has
			// one deadline. A connected helper can no longer hold this worker
			// forever without identifying itself.
			const auto helloDeadline = ::GetTickCount64() +
				osfui::wv2::kHelloTimeoutMs;
			std::string payload;
			json hello;
			for (int preHello = 0; ; ++preHello) {
				const auto now = ::GetTickCount64();
				if (now >= helloDeadline ||
					!pipe.ReadMessage(payload, static_cast<std::uint32_t>(
						helloDeadline - now))) {
					REX::ERROR("WebView2HostWebRenderer: helper connected but did not "
							   "complete hello in {}ms: {}",
						osfui::wv2::kHelloTimeoutMs, pipe.LastErrorText());
					LogHostStartFailureDiagnostics(launchTime);
					SignalDead("helper hello timed out");
					return;
				}
				hello = json::parse(payload, nullptr, false);
				if (preHello < 32 && hello.is_object() &&
					hello.value("type", "") == "log") {
					const auto level = hello.value("level", 0);
					const auto text = hello.value("text", "");
					if (level >= 2) {
						REX::ERROR("WebView2 host: {}", text);
					} else if (level == 1) {
						REX::WARN("WebView2 host: {}", text);
					} else {
						REX::INFO("WebView2 host: {}", text);
					}
					continue;
				}
				break;
			}

			const auto claimedPid =
				hello.is_object() ? hello.value("pid", 0u) : 0u;
			const auto protocol =
				hello.is_object() ? hello.value("protocolVersion", 0u) : 0u;
			if (!hello.is_object() || hello.value("type", "") != "hello" ||
				protocol != osfui::wv2::kProtocolVersion ||
				claimedPid != *peerPid) {
				REX::ERROR("WebView2HostWebRenderer: rejected helper hello "
						   "(protocol={}, claimed pid={}, kernel pid={}, host log: {})",
					protocol, claimedPid, *peerPid, hostLog.string());
				SignalDead("helper identity or protocol mismatch");
				return;
			}

			const HANDLE hostProcess = ::OpenProcess(
				SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
				FALSE, *peerPid);
			if (!hostProcess) {
				SignalDead(std::format("OpenProcess(helper pid {}) failed ({})",
					*peerPid, ::GetLastError()));
				return;
			}
			SetHostProcess(*peerPid, hostProcess);

			const auto runtimeVersion = hello.value("runtimeVersion", "?");
			if (runtimeVersion == "unknown") {
				REX::ERROR("WebView2HostWebRenderer: the WebView2 Evergreen runtime is not "
						   "installed; install it and restart the game");
			}
			REX::INFO("WebView2HostWebRenderer: verified host pid {} up "
					  "(WebView2 runtime {})", *peerPid, runtimeVersion);

			using OutItem = osfui::wv2::BoundedQueue<std::string>::Item;
			std::vector<OutItem> bootstrap;
			const auto addBootstrap = [&](json a_message) {
				bootstrap.push_back(OutItem{
					a_message.dump(), {},
					nextOutboundSequence.fetch_add(1, std::memory_order_relaxed) });
			};
			{
				std::scoped_lock lock(stateMutex);
				addBootstrap(json{
					{ "type", "init" },
					{ "topLevelHwnd", reinterpret_cast<std::uint64_t>(gameTopLevel) },
					{ "viewsPath", ToUtf8(mappedViewsRoot.native()) },
					{ "virtualHost", "osfui.local" },
					{ "width", width },
					{ "height", height },
					{ "userDataDir", ToUtf8(userData.native()) },
					{ "devMode", config.devMode },
					{ "hidden", allHidden },
					{ "adapterLuidLow", adapterLuidLow },
					{ "adapterLuidHigh", adapterLuidHigh },
				});
				addBootstrap(AccelStateMsg(accToggle,
					accCaptured, accArmed, accCaptureUp));
				accSent = true;
				for (const auto& view : views) {
					addBootstrap(NavigateMsg(view.id, view.entry,
						view.bridge, view.legacyApi, view.logicalHeight));
					if (view.prewarm) addBootstrap(PrewarmMsg(view.id));
					addBootstrap(SetHiddenMsg(
						view.id, view.hidden, presentationEpoch));
					addBootstrap(SetOrderMsg(view.id, view.order));
				}
				if (!activeId.empty()) addBootstrap(SetActiveMsg(activeId));
				addBootstrap(FocusMsg(focusRequested.load()));
			}
			if (!PublishConnected(std::move(bootstrap))) {
				if (!stopRequested.load(std::memory_order_acquire)) {
					SignalDead("connection snapshot exceeded the outbound queue limit");
				}
				return;
			}

			ReadLoop();
			connected.store(false, std::memory_order_release);
			if (!stopRequested.load(std::memory_order_acquire) &&
				!dead.load(std::memory_order_acquire)) {
				SignalDead("helper connection ended unexpectedly: " +
					pipe.LastErrorText());
			}
		}
		// Inbound (worker thread)

		void ReadLoop()
		{
			std::string payload;
			auto heartbeatDeadline = ::GetTickCount64() +
				osfui::wv2::kHeartbeatTimeoutMs;
			while (!stopRequested.load(std::memory_order_acquire)) {
				const auto now = ::GetTickCount64();
				if (now >= heartbeatDeadline) {
					REX::ERROR("WebView2HostWebRenderer: helper heartbeat expired after {}ms",
						osfui::wv2::kHeartbeatTimeoutMs);
					return;
				}
				if (!pipe.ReadMessage(payload, static_cast<std::uint32_t>(
						heartbeatDeadline - now))) {
					return;
				}
				const json msg = json::parse(payload, nullptr, false);
				if (msg.is_discarded()) continue;
				try {
					const std::string type = msg.value("type", "");
					if (type == "heartbeat") {
						heartbeatDeadline = ::GetTickCount64() +
							osfui::wv2::kHeartbeatTimeoutMs;
						continue;
					}
					if (type == "frame") {
						OnFrameMessage(msg);
					} else if (type == "textures") {
						OnTexturesMessage(msg);
					} else if (type == "webMessage") {
						Push(Notify{ .kind = Notify::Kind::Web,
							.view = msg.value("view", ""),
							.text = msg.value("json", "") });
					} else if (type == "loadEvent") {
						Push(Notify{ .kind = Notify::Kind::Load,
							.view = msg.value("view", ""),
							.text = msg.value("url", ""),
							.detail = msg.value("description", ""),
							.failed = msg.value("failed", false),
							.code = msg.value("code", 0) });
					} else if (type == "fatal") {
						Push(Notify{ .kind = Notify::Kind::Fatal,
							.view = msg.value("view", ""),
							.text = msg.value("stage", "renderer"),
							.detail = msg.value("description", "terminal renderer failure"),
							.unsignedCode = msg.value("code", 0u) });
					} else if (type == "console") {
						Push(Notify{ .kind = Notify::Kind::Console,
							.view = msg.value("view", ""),
							.text = msg.value("json", "") });
					} else if (type == "cursor") {
						// Contract allows renderer-thread delivery for cursor.
						if (onCursorChange) {
							onCursorChange(CursorShapeFromSystemCursorId(msg.value("id", 0u)));
						}
					} else if (type == "accelerator") {
						// Invoked off the game thread; the handler must stay cheap.
						if (onAccelerator) {
							onAccelerator(msg.value("vk", 0u), msg.value("scan", 0u),
								msg.value("down", false));
						}
					} else if (type == "log") {
						Push(Notify{ .kind = Notify::Kind::Log,
							.text = msg.value("text", ""),
							.code = msg.value("level", 0) });
					} else if (type == "ready" || type == "hello") {
						// informational
					} else if (type == "bye") {
						REX::INFO("WebView2HostWebRenderer: host bye ({})",
							msg.value("reason", ""));
					}
				} catch (const std::exception& e) {
					// A malformed / partial / version-mismatched host message must not
					// unwind out of the worker thread (that calls std::terminate).
					REX::WARN("WebView2HostWebRenderer: dropping malformed host message: {}", e.what());
				}
			}
		}

		void OnTexturesMessage(const json& a_msg)
		{
			// Ring depth comes from the announcement, not a compiled constant —
			// the host may retune it as long as it fits our capacity.
			static_assert(osfui::wv2::kRingSlots <= SharedRingDesc::kMaxSlots);
			SharedRingDesc desc{};
			const auto& slots = a_msg.at("slots");
			for (std::size_t i = 0; i < SharedRingDesc::kMaxSlots && i < slots.size(); ++i) {
				desc.slotHandles[i] = reinterpret_cast<void*>(
					static_cast<std::uintptr_t>(slots[i].get<std::uint64_t>()));
				++desc.slotCount;
			}
			for (std::size_t i = SharedRingDesc::kMaxSlots; i < slots.size(); ++i) {
				// Announced more than we can hold (mismatched builds — the
				// launcher's versioned mirror should prevent this). Close the
				// already-duplicated handles so they don't leak; frames landing
				// in those slots will not be drawn.
				::CloseHandle(reinterpret_cast<HANDLE>(
					static_cast<std::uintptr_t>(slots[i].get<std::uint64_t>())));
			}
			if (slots.size() > SharedRingDesc::kMaxSlots) {
				REX::WARN("WebView2HostWebRenderer: host announced {} ring slots, "
						  "capacity is {} — excess slots ignored",
					slots.size(), SharedRingDesc::kMaxSlots);
			}
			// Health edge for the System Health pane. Reported from Update() on
			// the game thread, not from here (this runs on the reader thread).
			ringSlotsAnnounced.store(static_cast<std::uint32_t>(slots.size()),
				std::memory_order_relaxed);
			desc.produceFence = reinterpret_cast<void*>(
				static_cast<std::uintptr_t>(a_msg.value("produceFence", 0ull)));
			desc.consumeFence = reinterpret_cast<void*>(
				static_cast<std::uintptr_t>(a_msg.value("consumeFence", 0ull)));
			desc.width = a_msg.value("width", 0u);
			desc.height = a_msg.value("height", 0u);
			desc.adapterLuidLow = a_msg.value("adapterLuidLow", 0u);
			desc.adapterLuidHigh = a_msg.value("adapterLuidHigh", 0u);
			{
				std::scoped_lock lock(frameMutex);
				desc.generation = ++ringGeneration;
				ringWidth = desc.width;
				ringHeight = desc.height;
				haveFrame = false;  // prior slots are invalid now
			}
			Push(Notify{ .kind = Notify::Kind::Ring, .ring = desc });
		}

		void OnFrameMessage(const json& a_msg)
		{
			const auto slot = a_msg.value("slot", 0u);
			const auto serial = a_msg.value("serial", 0ull);
			const auto presentation = a_msg.value("presentationEpoch", 0ull);
			const auto w = a_msg.value("width", 0u);
			const auto h = a_msg.value("height", 0u);
			std::uint64_t ackSerial = 0;
			bool ackNew = false;
			{
				std::scoped_lock lock(frameMutex, stateMutex);
				if (w != ringWidth || h != ringHeight) {
					ackNew = true;  // stale ring — release the slot immediately
				} else if (allHidden || presentation != presentationEpoch) {
					// Frames captured while closed, before the host completed
					// this reveal, or from an earlier open are not renderable.
					// Invalidate the cached frame as well: otherwise the next
					// open can mistake transparent closed-state pixels for its
					// required post-open frame.
					haveFrame = false;
					ackNew = true;
				} else {
					if (haveFrame && frameSerial != submittedSerial) {
						// The previous frame never reached the compositor, so
						// nothing will signal its consumption — ack it.
						ackSerial = frameSerial;
					}
					frameSlot = slot;
					frameSerial = serial;
					frameWidth = w;
					frameHeight = h;
					sharedRingGeneration = ringGeneration;
					haveFrame = true;
				}
			}
			if (ackSerial) {
				pipe.WriteMessage(FrameAckMsg(ackSerial).dump());
			}
			if (ackNew) {
				pipe.WriteMessage(FrameAckMsg(serial).dump());
			}
		}

		// Game-thread dispatch

		void DrainNotifications()
		{
			std::deque<Notify> local;
			std::size_t droppedWeb = 0;
			std::size_t droppedConsole = 0;
			std::size_t droppedLogs = 0;
			{
				std::scoped_lock lock(notifyMutex);
				local.swap(notifications);
				pendingWebCount = 0;
				pendingConsoleCount = 0;
				pendingLogCount = 0;
				droppedWeb = std::exchange(droppedWebCount, 0);
				droppedConsole = std::exchange(droppedConsoleCount, 0);
				droppedLogs = std::exchange(droppedLogCount, 0);
			}
			if (droppedWeb) {
				REX::WARN("WebView2HostWebRenderer: dropped {} web message(s) over the {}-message pending cap", droppedWeb, kMaxPendingWeb);
			}
			if (droppedConsole) {
				REX::WARN("WebView2HostWebRenderer: dropped {} console message(s) over the {}-message pending cap", droppedConsole, kMaxPendingConsole);
			}
			if (droppedLogs) {
				REX::WARN("WebView2HostWebRenderer: dropped {} host log line(s) over "
						  "the {}-line pending cap", droppedLogs, kMaxPendingLogs);
			}
			for (auto& value : local) {
				switch (value.kind) {
				case Notify::Kind::Web:
					{
						bool bridge = false;
						{
							std::scoped_lock lock(stateMutex);
							if (const auto* view = FindView(value.view)) bridge = view->bridge;
						}
						// Guarded for the same reason the ReadLoop below is: this
						// runs on the game thread's drain with no handler above it,
						// so anything escaping the bridge — a json throw on
						// view-supplied text, a handler bug — is a std::terminate.
						// One bad message must not take the process with it.
						try {
							if (onWebMessage && bridge) onWebMessage(value.view, value.text);
						} catch (const std::exception& e) {
							REX::ERROR("WebView2HostWebRenderer: web message from '{}' threw: {}",
								value.view, e.what());
						} catch (...) {
							REX::ERROR("WebView2HostWebRenderer: web message from '{}' threw a non-std exception",
								value.view);
						}
					}
					break;
				case Notify::Kind::Load:
					if (onLoad) {
						const LoadEvent event{
							.viewId = value.view,
							.failed = value.failed,
							.url = value.text,
							.description = value.detail,
							.errorDomain = "WebView2Host",
							.errorCode = value.code
						};
						onLoad(event);
					}
					break;
				case Notify::Kind::Fatal:
					if (onFailure) {
						onFailure(FailureEvent{
							.stage = value.text,
							.viewId = value.view,
							.description = value.detail,
							.errorCode = value.unsignedCode
						});
					}
					break;
				case Notify::Kind::Console:
					DeliverConsole(value.view, value.text);
					break;
				case Notify::Kind::Ring:
					announcedGeneration = value.ring.generation;
					if (onSharedRing) {
						onSharedRing(value.ring);
					} else {
						// Nobody adopts the handles; close them or they leak.
						for (auto* handle : value.ring.slotHandles) {
							if (handle) ::CloseHandle(handle);
						}
						if (value.ring.produceFence) ::CloseHandle(value.ring.produceFence);
						if (value.ring.consumeFence) ::CloseHandle(value.ring.consumeFence);
					}
					break;
				case Notify::Kind::Log:
					if (value.code >= 2) {
						REX::ERROR("WebView2 host: {}", value.text);
					} else if (value.code == 1) {
						REX::WARN("WebView2 host: {}", value.text);
					} else {
						REX::INFO("WebView2 host: {}", value.text);
					}
					break;
				case Notify::Kind::Dead:
					if (!deadLogged) {
						deadLogged = true;
						REX::ERROR("WebView2HostWebRenderer: host connection lost — the "
								   "overlay is closing before bounded helper recovery begins "
								   "(host log: {})", hostLog.string());
						if (onFailure) {
							onFailure(FailureEvent{
								.stage = "host-connection",
								.viewId = activeId,
								.description = "WebView2 host connection lost",
								.errorCode = 0
							});
						}
					}
					break;
				}
			}
		}

		void DeliverConsole(const std::string& a_view, const std::string& a_payload)
		{
			const auto found = consoleHandlers.find(a_view);
			if (found == consoleHandlers.end() || !found->second) return;
			int level = 0;
			std::string text = a_payload;
			try {
				const auto parsed = json::parse(a_payload);
				const auto type = parsed.value("type", "log");
				level = type == "warning" ? 1 : type == "error" ? 2 :
					type == "debug" ? 3 : type == "info" ? 4 : 0;
				if (const auto it = parsed.find("args");
					it != parsed.end() && it->is_array() && !it->empty()) {
					const auto& first = it->front();
					text = first.value("value", first.value("description", a_payload));
				}
			} catch (...) {}
			found->second(level, std::move(text));
		}

		// Teardown

		bool RequestShutdown()
		{
			std::uint64_t sequence = 0;
			{
				std::scoped_lock gateLock(writerGateMutex);
				if (!connected.exchange(false, std::memory_order_acq_rel)) return false;
				outbound.Clear();
				sequence =
					nextOutboundSequence.fetch_add(1, std::memory_order_relaxed);
				if (outbound.Push(json{ { "type", "shutdown" } }.dump(), {}, sequence) !=
					decltype(outbound)::PushResult::Queued) {
					return false;
				}
			}
			std::unique_lock lock(writtenMutex);
			return written.wait_for(lock, std::chrono::milliseconds(250), [this, sequence] {
				return lastWrittenSequence >= sequence || writerFailed;
			}) && lastWrittenSequence >= sequence;
		}

		void Stop(bool a_force = false)
		{
			const auto prior = lifecycle.exchange(
				Lifecycle::Stopping, std::memory_order_acq_rel);
			if (prior == Lifecycle::Stopped) return;

			stopRequested.store(true, std::memory_order_release);
			const bool shutdownWritten = RequestShutdown();
			if (!shutdownWritten && prior == Lifecycle::Running && !a_force) {
				REX::WARN("WebView2HostWebRenderer: shutdown message was not written "
						  "within 250ms; closing the transport");
			}

			outbound.Close();
			writerGate.notify_all();
			// Close cancels pending accept/read/write calls before either join.
			// No game-thread pipe operation is allowed to wait indefinitely.
			pipe.Close();
			if (worker.joinable()) worker.join();
			if (writer.joinable()) writer.join();
			connected.store(false, std::memory_order_release);
			{
				std::scoped_lock lock(writerGateMutex);
				writerReady = false;
			}

			if (const HANDLE hostProcess = TakeHostProcess()) {
				const auto graceMs = a_force ? 250u : 3000u;
				if (::WaitForSingleObject(hostProcess, graceMs) != WAIT_OBJECT_0) {
					REX::WARN("WebView2HostWebRenderer: verified host pid {} did not exit{}; terminating",
						::GetProcessId(hostProcess),
						a_force ? " after transport failure" : " in 3s");
					::TerminateProcess(hostProcess, 9);
					::WaitForSingleObject(hostProcess, 1000u);
				}
				::CloseHandle(hostProcess);
			}

			lifecycle.store(Lifecycle::Stopped, std::memory_order_release);
			// The host and browser are gone, so their per-run real-path view tree
			// is no longer needed.
			{
				std::scoped_lock mirrorLock(viewsMirrorMutex);
				if (usesViewsMirror && mappedViewsRoot != viewsRoot) {
					std::error_code ec;
					std::filesystem::remove_all(mappedViewsRoot, ec);
					if (ec) {
						REX::DEBUG("WebView2HostWebRenderer: per-run views mirror cleanup "
								   "deferred to the OS ({})", ec.message());
					}
					usesViewsMirror = false;
				}
			}
		}
		void ResetAfterFailure()
		{
			// The failure notification is drained from Update after ReadLoop has
			// ended, so this forced stop normally joins an already-finished worker.
			// If a stranded helper kept running after the pipe died, do not stall
			// Starfield's main thread waiting for a graceful process exit.
			Stop(true);

			if (ringSlotsReported > SharedRingDesc::kMaxSlots) {
				ReportHealth("host.ring-truncated", false);
			}

			const auto discardedOut = outbound.Size();
			outbound.Reset();
			{
				std::scoped_lock lock(writerGateMutex);
				writerReady = false;
			}
			{
				std::scoped_lock lock(writtenMutex);
				lastWrittenSequence = 0;
				writerFailed = false;
			}
			outboundOverflowed.store(false, std::memory_order_release);

			{
				std::scoped_lock lock(notifyMutex);
				for (auto& value : notifications) {
					if (value.kind != Notify::Kind::Ring) {
						continue;
					}
					for (auto*& handle : value.ring.slotHandles) {
						if (handle) ::CloseHandle(handle);
					}
					if (value.ring.produceFence) ::CloseHandle(value.ring.produceFence);
					if (value.ring.consumeFence) ::CloseHandle(value.ring.consumeFence);
				}
				notifications.clear();
				pendingWebCount = pendingConsoleCount = pendingLogCount = 0;
				droppedWebCount = droppedConsoleCount = droppedLogCount = 0;
			}

			{
				std::scoped_lock lock(frameMutex);
				haveFrame = false;
				frameSlot = 0;
				frameSerial = 0;
				frameWidth = frameHeight = 0;
				sharedRingGeneration = 0;
				submittedSerial = 0;
				ringWidth = ringHeight = 0;
				announcedGeneration = 0;
				// Keep ringGeneration monotonic across helper processes so a new
				// shared ring is unambiguously newer than the compositor's retired
				// generation.
			}
			{
				std::scoped_lock lock(stateMutex);
				accSent = false;
			}

			connected.store(false, std::memory_order_release);
			dead.store(false, std::memory_order_release);
			deadLogged = false;
			focusRequested.store(false);
			focusCheckAccum = 0.0;
			focusFixWarned = false;
			ringSlotsAnnounced.store(0);
			ringSlotsReported = 0;
			stopRequested.store(false);

			if (discardedOut) {
				REX::WARN("WebView2HostWebRenderer: discarded {} transient message(s) from "
						  "the dead document; runtime state will be replayed to its replacement",
					discardedOut);
			}
		}
	};

	WebView2HostWebRenderer::WebView2HostWebRenderer() :
		_impl(std::make_unique<Impl>())
	{}

	WebView2HostWebRenderer::~WebView2HostWebRenderer()
	{
		if (_impl) _impl->Stop();
	}

	bool WebView2HostWebRenderer::Initialize(const RendererConfig& a_config)
	{
		// No Evergreen-runtime probe here: detecting it in-process would link the
		// WebView2 SDK loader into this GPL'd plugin for one symbol. The host exe
		// already links the SDK and reports the runtime version in its hello, so
		// a missing runtime is diagnosed there.
		_impl->config = a_config;
		_impl->viewsRoot = a_config.dataDir / "views";
		_impl->userData = LocalOsfuiDir() / "WebView2";
		_impl->hostLog = HostLogPath();
		_impl->hostExeSource = a_config.dataDir / "bin" / "osfui_webview2_host.exe";
		_impl->width = (std::max)(1u, a_config.width);
		_impl->height = (std::max)(1u, a_config.height);
		std::error_code ec;
		if (!std::filesystem::exists(_impl->hostExeSource, ec)) {
			REX::ERROR("WebView2HostWebRenderer: {} is missing — the out-of-process "
					   "host was not packaged with this install",
				_impl->hostExeSource.string());
			return false;
		}
		return true;
	}

	bool WebView2HostWebRenderer::RestartAfterFailure()
	{
		if (!_impl) return false;
		_impl->ResetAfterFailure();
		return true;
	}

	void WebView2HostWebRenderer::LoadView(const ViewManifest& a_manifest)
	{
		// One expression for the clamp, so the stored value and the sent value
		// cannot drift.
		const auto logicalHeight = (std::max)(1u, a_manifest.height);
		{
			std::scoped_lock lock(_impl->stateMutex);
			auto* view = _impl->FindView(a_manifest.id);
			if (!view) {
				view = &_impl->views.emplace_back();
				view->id = a_manifest.id;
			}
			view->entry = a_manifest.entry;
			view->bridge = a_manifest.permissions.nativeBridge;
			view->legacyApi = IsPre2Target(a_manifest.targetVersion);
			view->logicalHeight = logicalHeight;
			// The first loaded view receives input until the runtime says
			// otherwise.
			if (_impl->activeId.empty()) {
				_impl->activeId = a_manifest.id;
			}
		}
		// A repeat LoadView for a live id re-navigates it (dev reload / crash
		// recovery).
		_impl->Send(NavigateMsg(a_manifest.id, a_manifest.entry,
			a_manifest.permissions.nativeBridge, IsPre2Target(a_manifest.targetVersion), logicalHeight));
	}

    bool WebView2HostWebRenderer::RefreshViewFiles(std::string_view a_viewId)
    {
        return _impl && _impl->RefreshViewFiles(a_viewId);
    }

	void WebView2HostWebRenderer::SetActiveView(std::string_view a_id)
	{
		{
			std::scoped_lock lock(_impl->stateMutex);
			if (!_impl->FindView(a_id)) {
				REX::WARN("WebView2HostWebRenderer: SetActiveView('{}') ignored — view not loaded",
					a_id);
				return;
			}
			if (_impl->activeId == a_id) return;
			_impl->activeId = a_id;
		}
		_impl->Send(SetActiveMsg(a_id));
	}

	void WebView2HostWebRenderer::Resize(std::uint32_t a_width, std::uint32_t a_height)
	{
		if (!a_width || !a_height) return;
		{
			std::scoped_lock lock(_impl->stateMutex);
			_impl->width = a_width;
			_impl->height = a_height;
		}
		_impl->Send(json{
			{ "type", "resize" }, { "width", a_width }, { "height", a_height } });
	}

	void WebView2HostWebRenderer::Update(double a_deltaSeconds)
	{
		// Warm start once a view is configured: mirror copies, broker launch,
		// environment creation and navigation all happen while the overlay is
		// still hidden.
		if (_impl->lifecycle.load(std::memory_order_acquire) ==
			Impl::Lifecycle::Stopped && !_impl->dead.load()) {
			bool wantsView = false;
			{
				std::scoped_lock lock(_impl->stateMutex);
				wantsView = !_impl->views.empty();
			}
			if (wantsView) _impl->Start();
		}
		_impl->DrainNotifications();

		// Focus watchdog (see the Impl member note for the race this heals).
		// GetGUIThreadInfo(0) reports the foreground thread's focus window: while
		// the user is alt-tabbed away that window is outside the game window's
		// tree, so both branches no-op and focus is never stolen from another
		// application.
		const auto hostSession = _impl->SessionSnapshot();
		if (hostSession.topLevel && _impl->connected.load(std::memory_order_acquire)) {
			_impl->focusCheckAccum += a_deltaSeconds;
			if (_impl->focusCheckAccum >= 0.5) {
				_impl->focusCheckAccum = 0.0;
				GUITHREADINFO info{};
				info.cbSize = sizeof(info);
				bool healthy = true;
				if (::GetGUIThreadInfo(0, &info) && info.hwndFocus) {
					DWORD focusPid = 0;
					::GetWindowThreadProcessId(info.hwndFocus, &focusPid);
					const bool inGameTree = info.hwndFocus == hostSession.topLevel ||
											::IsChild(hostSession.topLevel, info.hwndFocus) != FALSE;
					const bool focusInHost = focusPid != ::GetCurrentProcessId();
					if (!_impl->focusRequested && inGameTree && focusInHost) {
						// No interactive-menu session is live, but focus is stranded in
						// the host's Chromium child: keyboard, raw mouse AND gamepad
						// (WGI) are all dead for the game.
						healthy = false;
						if (!_impl->focusFixWarned) {
							_impl->focusFixWarned = true;
							REX::WARN("WebView2HostWebRenderer: focus stranded in host child "
									  "0x{:X} outside an interactive menu; re-asserting game focus (watchdog)",
								reinterpret_cast<std::uintptr_t>(info.hwndFocus));
						}
						::PostMessageW(hostSession.topLevel,
							OverlayInputHook::kRestoreGameFocusMessage, 0, 0);
					} else if (_impl->focusRequested && info.hwndFocus == hostSession.topLevel) {
						// Interactive-menu session live but Chromium never took
						// focus (MoveFocus lost): input and foreground scheduling
						// would remain on the wrong process.
						healthy = false;
						if (!_impl->focusFixWarned) {
							_impl->focusFixWarned = true;
							REX::WARN("WebView2HostWebRenderer: interactive menu live but game window "
									  "still owns focus; re-sending focus request (watchdog)");
						}
						_impl->Send(FocusMsg(true));
					}
				}
				if (healthy) {
					_impl->focusFixWarned = false;
				}
				// Deliberately NOT reported to System Health. This condition is
				// self-correcting within a tick or two — the watchdog above is the
				// fix, not a mitigation someone has to act on — so a card told the
				// player about an internal mechanism they cannot influence and
				// which had already resolved by the time they read it. The WARN
				// above remains for log-based diagnosis.
			}
		}

		// Ring-depth edge, moved off the reader thread (see ringSlotsAnnounced).
		// A truncated ring still renders — frames landing in the slots past our
		// capacity are skipped — so this is a degradation, not a failure.
		if (const auto announced = _impl->ringSlotsAnnounced.exchange(0, std::memory_order_relaxed);
			announced != 0 && announced != _impl->ringSlotsReported) {
			_impl->ringSlotsReported = announced;
			const bool truncated = announced > SharedRingDesc::kMaxSlots;
			const auto detail = truncated ?
				std::format("host announced {} slots, capacity {}",
					announced, SharedRingDesc::kMaxSlots) :
				std::string{};
			_impl->ReportHealth("host.ring-truncated", truncated, detail);
		}
	}

	std::optional<FrameBufferView> WebView2HostWebRenderer::Render()
	{
		std::scoped_lock lock(_impl->frameMutex);
		if (!_impl->haveFrame ||
			_impl->sharedRingGeneration != _impl->announcedGeneration) {
			// No frame, or its ring is not yet announced to the compositor
			// (the Ring notification dispatches from Update()).
			return std::nullopt;
		}
		_impl->submittedSerial = _impl->frameSerial;
		return FrameBufferView{
			.width = _impl->frameWidth,
			.height = _impl->frameHeight,
			.frameIndex = _impl->frameSerial,
			.sharedSlot = _impl->frameSlot,
		};
	}

	void WebView2HostWebRenderer::SendMessageToWeb(
		std::string_view a_viewId, std::string_view a_json)
	{
		{
			std::scoped_lock lock(_impl->stateMutex);
			const auto* view = _impl->FindView(a_viewId);
			if (!view || !view->bridge) return;
		}
		_impl->SendOrQueue(json{ { "type", "postWeb" },
			{ "view", std::string(a_viewId) }, { "json", std::string(a_json) } });
	}

	void WebView2HostWebRenderer::SetWebMessageHandler(WebMessageHandler a_handler)
	{
		_impl->onWebMessage = std::move(a_handler);
	}
	void WebView2HostWebRenderer::SetLoadHandler(LoadHandler a_handler)
	{
		_impl->onLoad = std::move(a_handler);
	}
	void WebView2HostWebRenderer::SetFailureHandler(FailureHandler a_handler)
	{
		_impl->onFailure = std::move(a_handler);
	}
	void WebView2HostWebRenderer::SetCursorChangeHandler(CursorChangeHandler a_handler)
	{
		_impl->onCursorChange = std::move(a_handler);
	}
	void WebView2HostWebRenderer::SetNativeAcceleratorHandler(
		NativeAcceleratorHandler a_handler)
	{
		_impl->onAccelerator = std::move(a_handler);
	}
	void WebView2HostWebRenderer::SetSharedRingHandler(SharedRingHandler a_handler)
	{
		_impl->onSharedRing = std::move(a_handler);
	}
	void WebView2HostWebRenderer::SetHealthHandler(HealthHandler a_handler)
	{
		_impl->onHealth = std::move(a_handler);
	}

	void WebView2HostWebRenderer::SetNativeFocus(bool a_focused)
	{
		if (a_focused) {
			_impl->Start();
		}
		_impl->focusRequested = a_focused;
		_impl->Send(FocusMsg(a_focused));
		const auto hostSession = _impl->SessionSnapshot();
		if (!a_focused && hostSession.topLevel) {
			// Restore game focus on the game's own window thread.
			::PostMessageW(hostSession.topLevel,
				OverlayInputHook::kRestoreGameFocusMessage, 0, 0);
		}
	}

	void WebView2HostWebRenderer::SetAcceleratorKeys(std::uint32_t a_toggleScan,
		bool a_captured, bool a_captureArmed,
		std::uint32_t a_captureUpScan)
	{
		bool changed = false;
		{
			std::scoped_lock lock(_impl->stateMutex);
			changed = !_impl->accSent || _impl->accToggle != a_toggleScan ||
				_impl->accCaptured != a_captured ||
				_impl->accArmed != a_captureArmed ||
				_impl->accCaptureUp != a_captureUpScan;
			_impl->accToggle = a_toggleScan;
			_impl->accCaptured = a_captured;
			_impl->accArmed = a_captureArmed;
			_impl->accCaptureUp = a_captureUpScan;
			if (changed && _impl->connected.load()) _impl->accSent = true;
		}
		if (changed) {
			_impl->Send(AccelStateMsg(a_toggleScan, a_captured,
				a_captureArmed, a_captureUpScan));
		}
	}

	void WebView2HostWebRenderer::NotifyPlayerCloseRequest()
	{
		// No queueing: if the host is not connected yet there is no crash
		// prompt to suppress, and replaying a stale close request at connect
		// time could mask a later real crash.
		_impl->Send(json{ { "type", "playerCloseRequest" } });
	}

	void WebView2HostWebRenderer::InjectKeyEvent(std::uint32_t a_vkCode, bool a_down)
	{
		// Dispatched into the active page as a DOM KeyboardEvent by the host shim.
		// Used for gamepad nav taps and Esc back-delegation; physical keyboard and
		// IME input route natively while the interactive menu owns focus.
		_impl->Send(json{ { "type", "key" }, { "vk", a_vkCode }, { "down", a_down } });
	}

	void WebView2HostWebRenderer::InjectMouseMove(int a_x, int a_y)
	{
		_impl->Send(json{ { "type", "mouse" }, { "kind", "move" },
			{ "x", a_x }, { "y", a_y } });
	}
	void WebView2HostWebRenderer::InjectMouseButton(
		int a_x, int a_y, int a_button, bool a_down)
	{
		_impl->Send(json{ { "type", "mouse" }, { "kind", "button" },
			{ "x", a_x }, { "y", a_y }, { "button", a_button }, { "down", a_down } });
	}
	void WebView2HostWebRenderer::InjectMouseWheel(int a_x, int a_y, int a_wheelDelta)
	{
		_impl->Send(json{ { "type", "mouse" }, { "kind", "wheel" },
			{ "x", a_x }, { "y", a_y }, { "wheel", a_wheelDelta } });
	}

	void WebView2HostWebRenderer::InjectPhysicalMouseWheel(
		int a_x, int a_y, int a_wheelDelta)
	{
		_impl->Send(json{ { "type", "mouse" }, { "kind", "physicalWheel" },
			{ "x", a_x }, { "y", a_y }, { "wheel", a_wheelDelta } });
	}

	void WebView2HostWebRenderer::OpenDevTools(std::string_view a_viewId)
	{
		_impl->Send(ViewMsg("openDevTools", a_viewId));
	}

	void WebView2HostWebRenderer::SetConsoleHandler(
		std::string_view a_viewId, ConsoleHandler a_handler)
	{
		if (a_handler) {
			_impl->consoleHandlers[std::string(a_viewId)] = std::move(a_handler);
		} else {
			_impl->consoleHandlers.erase(std::string(a_viewId));
		}
	}

	void WebView2HostWebRenderer::SetViewHidden(std::string_view a_viewId, bool a_hidden)
	{
		std::uint64_t presentation = 0;
		{
			std::scoped_lock lock(_impl->frameMutex, _impl->stateMutex);
			auto* view = _impl->FindView(a_viewId);
			if (!view) return;
			const bool wasAllHidden = _impl->allHidden;
			view->hidden = a_hidden;
			_impl->RecomputeAllHidden();
			if (wasAllHidden && !_impl->allHidden) {
				++_impl->presentationEpoch;
				_impl->haveFrame = false;
			} else if (_impl->allHidden) {
				_impl->haveFrame = false;
			}
			presentation = _impl->presentationEpoch;
		}
		_impl->Send(SetHiddenMsg(a_viewId, a_hidden, presentation));
	}

	void WebView2HostWebRenderer::PrewarmView(std::string_view a_viewId)
	{
		{
			std::scoped_lock lock(_impl->stateMutex);
			auto* view = _impl->FindView(a_viewId);
			if (!view || view->prewarm) return;
			view->prewarm = true;
		}
		_impl->Send(PrewarmMsg(a_viewId));
	}

	void WebView2HostWebRenderer::SuspendView(std::string_view a_viewId)
	{
		{
			std::scoped_lock lock(_impl->stateMutex);
			if (!_impl->FindView(a_viewId)) return;
		}
		_impl->Send(SuspendMsg(a_viewId));
	}

	void WebView2HostWebRenderer::SetViewOrder(std::string_view a_viewId, int a_order)
	{
		{
			std::scoped_lock lock(_impl->stateMutex);
			auto* view = _impl->FindView(a_viewId);
			if (!view) return;
			if (view->order == a_order) return;
			view->order = a_order;
		}
		_impl->Send(SetOrderMsg(a_viewId, a_order));
	}

	void WebView2HostWebRenderer::DestroyView(std::string_view a_viewId)
	{
		{
			std::scoped_lock lock(_impl->stateMutex);
			const auto* view = _impl->FindView(a_viewId);
			if (!view) return;
			std::erase_if(_impl->views,
				[&](const Impl::ViewRec& a_rec) { return a_rec.id == a_viewId; });
			if (_impl->activeId == a_viewId) {
				_impl->activeId = _impl->views.empty() ? std::string{} : _impl->views.front().id;
			}
			_impl->RecomputeAllHidden();
		}
		// Game-thread map; no lock.
		_impl->consoleHandlers.erase(std::string(a_viewId));
		_impl->Send(json{ { "type", "destroyView" }, { "view", std::string(a_viewId) } });
	}
}
