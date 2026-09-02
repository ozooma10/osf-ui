#include "Render/WebView2HostWebRenderer.h"

#include <atomic>
#include <deque>
#include <limits>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Composite/EngineD3D12.h"
#include "Core/Log.h"
#include "Core/Version.h"
#include "Views/Dev/DevViewFiles.h"
#include "Views/ViewCache.h"
#include "Core/Json.h"
#include "Input/OverlayInputHook.h"

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
#include "Wv2Messages.h"
#include "Wv2Pipe.h"
#include "Wv2Protocol.h"
#include "Win32Util.h"

using nlohmann::json;

// Private message used to return unsolicited browser focus to the game window.
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

		// Use the real SFSE log directory shared by the plugin and unhooked browser host.
		std::filesystem::path BrowserHostLogPath()
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

		bool BrowserHostProcessRunning()
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

		class ScopedCacheMutex
		{
		public:
			ScopedCacheMutex()
			{
				m_handle = ::CreateMutexW(nullptr, FALSE, ViewCache::kMutexName);
				if (!m_handle) return;
				const auto wait = ::WaitForSingleObject(m_handle, 30000);
				m_owned = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
			}
			~ScopedCacheMutex()
			{
				if (m_owned) ::ReleaseMutex(m_handle);
				if (m_handle) ::CloseHandle(m_handle);
			}

			[[nodiscard]] bool Owned() const { return m_owned; }

		private:
			HANDLE m_handle{ nullptr };
			bool   m_owned{ false };
		};

		HANDLE AcquireViewCacheLease(const std::filesystem::path& a_generation)
		{
			const auto lock = a_generation / ViewCache::kUseLock;
			return ::CreateFileW(lock.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_HIDDEN, nullptr);
		}

		bool CacheGenerationCanBeRemoved(const std::filesystem::path& a_generation)
		{
			const auto lock = a_generation / ViewCache::kUseLock;
			std::error_code ec;
			if (!std::filesystem::exists(lock, ec)) {
				return !ec;  // abandoned staging tree before the lease file was created
			}
			const HANDLE probe = ::CreateFileW(lock.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (probe == INVALID_HANDLE_VALUE) {
				return false;  // an active game/host lease denies delete sharing
			}
			::CloseHandle(probe);
			return true;
		}

		bool ProcessIsAlive(DWORD a_pid)
		{
			if (a_pid == 0) return false;
			const HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, a_pid);
			if (!process) {
				return ::GetLastError() != ERROR_INVALID_PARAMETER;
			}
			const bool alive = ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
			::CloseHandle(process);
			return alive;
		}

		std::optional<DWORD> SessionMirrorPid(
			std::string_view a_name, std::string_view a_prefix)
		{
			if (!a_name.starts_with(a_prefix)) return std::nullopt;
			const auto digits = a_name.substr(a_prefix.size());
			if (digits.empty()) return std::nullopt;
			std::uint64_t value = 0;
			for (const unsigned char ch : digits) {
				if (ch < '0' || ch > '9') return std::nullopt;
				value = value * 10 + (ch - '0');
				if (value > std::numeric_limits<DWORD>::max()) return std::nullopt;
			}
			return static_cast<DWORD>(value);
		}

		std::size_t ScavengeLegacyViewMirrors(const std::filesystem::path& a_localRoot)
		{
			std::size_t removed = 0;
			std::error_code ec;
			const bool anotherHostRunning = BrowserHostProcessRunning();
			for (std::filesystem::directory_iterator it(a_localRoot, ec), end;
				 !ec && it != end; it.increment(ec)) {
				if (!it->is_directory(ec)) {
					if (ec) break;
					continue;
				}
				const auto name = it->path().filename().string();
				if (name == "views-mirror") {
					if (anotherHostRunning) continue;
				} else if (const auto legacyPid = SessionMirrorPid(name, "views-mirror-")) {
					if (ProcessIsAlive(*legacyPid)) continue;
				} else if (const auto devPid = SessionMirrorPid(name, "views-dev-")) {
					if (ProcessIsAlive(*devPid)) continue;
				} else {
					continue;
				}
				std::error_code removeEc;
				std::filesystem::remove_all(it->path(), removeEc);
				if (!removeEc) ++removed;
			}
			return removed;
		}

		// Read the browser-host log tail for pre-handshake failure diagnostics.
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

		// Build shared wire structs from copied values; never retain ViewRec or string_view across stateMutex.
		namespace msg = osfui::wv2::msg;
	}

	struct WebView2HostWebRenderer::Impl
	{
		struct Notify
		{
			enum class Kind { Web, Load, Fatal, Console, Ring, Log, Focus, Dead };
			Kind           kind{ Kind::Web };
			std::string    view;
			std::string    text, detail;
			bool           focused{};
			bool           failed{};
			int            code{};
			std::uint32_t  unsignedCode{};
			std::uint64_t  id{};
			std::uint64_t  sequence{};
			SharedRingDesc ring{};
		};

		WebView2HostConfig    config;
		std::filesystem::path viewsRoot, mappedViewsRoot, userData;
        // Serialize initial and dev-refresh writes to the real-path mirror.
        std::mutex            viewsMirrorMutex;
        bool                  usesViewsMirror{ false };
		bool                  removeViewsMirrorOnStop{ false };
		HANDLE                viewsCacheLease{ INVALID_HANDLE_VALUE };
		std::filesystem::path browserHostExeSource, browserHostExeMirror;
		std::filesystem::path browserHostLog;  // set in Initialize; read by worker + notify drain
		std::uint32_t adapterLuidLow{ 0 }, adapterLuidHigh{ 0 };
		bool          adapterLuidKnown{ false };

		WebMessageHandler       onWebMessage;
		LoadHandler             onLoad;
		FailureHandler          onFailure;
		CursorChangeHandler     onCursorChange;
		NativeAcceleratorHandler onAccelerator;
		RelativePointerHandler  onRelativePointer;
		SharedRingHandler       onSharedRing;
		HealthHandler           onHealth;
		// Game-thread only (Drain/setters).
		std::unordered_map<std::string, ConsoleHandler>    consoleHandlers;  // viewId -> cb

		// Preserve creation order for z ties; the worker snapshots state and callers send later diffs.
		struct ViewRec
		{
			std::string id;
			std::string entry;
			bool        hidden{ true };
			int         order{ 0 };
			// Authoring height defines browser rasterization scale against output height.
			std::uint32_t logicalHeight{ kDefaultViewHeight };
		};
		std::mutex           stateMutex;
		std::vector<ViewRec> views;
		std::string          inputTargetId;
		bool                 allHidden{ true };  // no visible view => frames are not taken
		// Each hidden-to-visible presentation requires a post-reveal frame.
		std::uint64_t        presentationEpoch{ 0 };
		std::uint32_t        width{ 1 }, height{ 1 };
		std::uint32_t        viewportWidth{ 1 }, viewportHeight{ 1 };
		bool                 pointerInputEnabled{ true };
		// accelState mirror (SetAcceleratorKeys diffs against this)
		std::uint32_t accToggle{ 0 }, accCaptureUp{ 0 };
		bool          accCaptured{ false }, accArmed{ false }, accSent{ false };
		std::string   relativePointerView;
		bool          relativePointerActive{ false };

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

		struct BrowserHostSession
		{
			DWORD  pid{ 0 };
			HANDLE process{ nullptr };
			HWND   topLevel{ nullptr };
		};
		std::mutex sessionMutex;
		BrowserHostSession session;

		// Focus requests cross a process boundary. Epochs and actual-state acknowledgements make transitions deterministic; the slow Win32 poll below remains a last-resort safety net.
		std::atomic_bool focusRequested{ false };  // last SetNativeFocus argument
		std::atomic<std::uint64_t> focusEpoch{ 0 };
		std::uint64_t focusAckEpoch{ 0 };
		std::uint64_t focusAckSequence{ 0 };
		bool          focusActual{ false };
		std::string   focusActualView;
		double focusCheckAccum{ 0.0 };
		double focusMismatchAccum{ 0.0 };
		bool   focusFixWarned{ false };  // one WARN per strand episode

		std::atomic<std::uint32_t> ringSlotsAnnounced{ 0 };
		std::uint32_t              ringSlotsReported{ 0 };

		void ReportHealth(std::string_view a_code, bool a_active, std::string_view a_detail = {})
		{
			if (onHealth) {
				onHealth(HealthEvent{ a_code, a_active, a_detail });
			}
		}

		std::mutex         notifyMutex;
		std::deque<Notify> notifications;
		// Independently bound page-provokable queues while game-thread draining is paused.
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
		std::uint64_t submittedRingGeneration{ 0 };
		std::uint64_t submittedSerial{ 0 };
		std::uint32_t ringWidth{ 0 }, ringHeight{ 0 };
		std::uint32_t ringSlotCount{ 0 };
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

		[[nodiscard]] BrowserHostSession BrowserHostSessionSnapshot()
		{
			std::scoped_lock lock(sessionMutex);
			return session;
		}

		void SetTopLevel(HWND a_topLevel)
		{
			std::scoped_lock lock(sessionMutex);
			session.topLevel = a_topLevel;
		}

		void SetBrowserHostProcess(DWORD a_pid, HANDLE a_process)
		{
			std::scoped_lock lock(sessionMutex);
			session.pid = a_pid;
			session.process = a_process;
		}

		HANDLE TakeBrowserHostProcess()
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
			return osfui::wv2::GameMessageCoalesceKey(
				Json::Get(a_msg, "type", ""),
				Json::Get(a_msg, "kind", ""),
				Json::Get(a_msg, "view", ""));
		}

		bool Enqueue(const json& a_msg, bool a_queueBeforeConnect)
		{
			std::scoped_lock gateLock(writerGateMutex);
			if (!a_queueBeforeConnect &&
				!connected.load(std::memory_order_acquire)) {
				return false;
			}
			const auto result = outbound.Push(Json::Dump(a_msg), CoalesceKey(a_msg),
				nextOutboundSequence.fetch_add(1, std::memory_order_relaxed));
			if (result == decltype(outbound)::PushResult::Full) {
				if (!outboundOverflowed.exchange(true)) {
					SignalDead(std::format(
						"outbound IPC message queue exceeded {} messages", kMaxOutbound));
				}
				return false;
			}
			return result != decltype(outbound)::PushResult::Closed;
		}

		void Send(const json& a_msg) { Enqueue(a_msg, false); }

		// Atomically place the connection snapshot before bounded queued events.
		void SendOrQueue(const json& a_msg) { Enqueue(a_msg, true); }

		bool PublishConnected(std::vector<osfui::wv2::BoundedQueue<std::string>::Item> a_bootstrap)
		{
			{
				// Share the lock with shutdown so Stop cannot miss a racing connection.
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
			bool writerActivated = false;
			{
				std::unique_lock lock(writerGateMutex);
				writerGate.wait(lock, [this] {
					return writerReady || stopRequested.load(std::memory_order_acquire);
				});
				writerActivated = writerReady;
			}
			if (!writerActivated) return;

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

		// Materialize views at a real path visible outside MO2's USVFS. Production uses immutable fingerprinted generations; 
		// developer hot reload gets a private mutable tree so it cannot alter a generation another game uses.
		bool ResolveMappedViewsRoot()
		{
			std::scoped_lock mirrorLock(viewsMirrorMutex);
			mappedViewsRoot = viewsRoot;
			usesViewsMirror = false;
			removeViewsMirrorOnStop = false;
			if (viewsCacheLease != INVALID_HANDLE_VALUE) {
				::CloseHandle(viewsCacheLease);
				viewsCacheLease = INVALID_HANDLE_VALUE;
			}
			if (!::GetModuleHandleW(L"usvfs_x64.dll")) return true;

			ScopedCacheMutex cacheMutex;
			if (!cacheMutex.Owned()) {
				REX::ERROR("WebView2HostWebRenderer: timed out acquiring the shared views-cache lock");
				return false;
			}

			const auto localRoot = LocalOsfuiDir();
			const auto started = std::chrono::steady_clock::now();
			if (config.devMode) {
				ScavengeLegacyViewMirrors(localRoot);
				const auto mirror = localRoot / std::format("views-dev-{}", ::GetCurrentProcessId());
				std::error_code ec;
				std::filesystem::remove_all(mirror, ec);
				if (ec) {
					REX::ERROR("WebView2HostWebRenderer: could not clear developer views mirror '{}' ({})", ToUtf8(mirror.native()), ec.message());
					return false;
				}
				std::string error;
				if (!DevViewFiles::SyncTree(viewsRoot, mirror, error)) {
					REX::ERROR("WebView2HostWebRenderer: developer views mirror failed ({})", error);
					return false;
				}
				{
					std::ofstream lock(mirror / ViewCache::kUseLock, std::ios::binary | std::ios::trunc);
					if (!lock) {
						REX::ERROR("WebView2HostWebRenderer: could not create developer views lease file");
						return false;
					}
				}
				viewsCacheLease = AcquireViewCacheLease(mirror);
				if (viewsCacheLease == INVALID_HANDLE_VALUE) {
					REX::ERROR("WebView2HostWebRenderer: could not lease developer views mirror ({})", ::GetLastError());
					return false;
				}
				mappedViewsRoot = mirror;
				usesViewsMirror = true;
				removeViewsMirrorOnStop = true;
				REX::INFO("WebView2HostWebRenderer: USVFS developer views mirrored to {}", ToUtf8(mirror.native()));
				return true;
			}

			const auto cacheRoot = localRoot / ViewCache::kCacheDirectory;
			const auto stagingId = std::format("{}-{}", ::GetCurrentProcessId(), ::GetTickCount64());
			std::string error;
			const auto prepared = ViewCache::Prepare(viewsRoot, cacheRoot, kOsfuiReleaseVersion, stagingId, error);
			if (!prepared) {
				REX::ERROR("WebView2HostWebRenderer: views cache preparation failed ({})", error);
				return false;
			}

			viewsCacheLease = AcquireViewCacheLease(prepared->generation);
			if (viewsCacheLease == INVALID_HANDLE_VALUE) {
				REX::ERROR("WebView2HostWebRenderer: could not lease views-cache generation '{}' ({})", ToUtf8(prepared->generation.native()), ::GetLastError());
				return false;
			}
			mappedViewsRoot = prepared->generation;
			usesViewsMirror = true;

			const auto scavenged = ViewCache::Scavenge(cacheRoot, mappedViewsRoot,
				[](const std::filesystem::path& a_generation) {
					return CacheGenerationCanBeRemoved(a_generation);
				});
			const auto legacyRemoved = ScavengeLegacyViewMirrors(localRoot);
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
			REX::INFO("WebView2HostWebRenderer: USVFS views cache {} {} ({} files, {:.2f} MiB, {} ms; removed {} old generation(s) + {} legacy mirror(s), retained {} generation(s))",
				prepared->reused ? "reused" : "published", ToUtf8(mappedViewsRoot.native()), prepared->fingerprint.files, static_cast<double>(prepared->fingerprint.bytes) / (1024.0 * 1024.0), elapsed, scavenged.removed, legacyRemoved, scavenged.retained);
			if (scavenged.failed) {
				REX::WARN("WebView2HostWebRenderer: {} views-cache item(s) could not be scavenged; they will be retried next launch", scavenged.failed);
			}
			return true;
		}

        // Refresh the real-path mod mirror before navigating an unhooked browser.
		bool RefreshViewFiles(std::string_view a_viewId)
		{
			if (!config.devMode) return true;
			std::scoped_lock mirrorLock(viewsMirrorMutex);
			if (!usesViewsMirror) return true;

			// Mirror the whole mod folder because view entries load sibling hashed assets.
			const auto modFolder = std::filesystem::path(DevViewFiles::ModFolder(a_viewId));
			const auto source = viewsRoot / modFolder;
			const auto destination = mappedViewsRoot / modFolder;
			std::string error;
			if (!DevViewFiles::SyncTree(source, destination, error)) {
				REX::WARN("WebView2HostWebRenderer: dev reload could not mirror '{}' ({})", a_viewId, error);
				return false;
			}
			return true;
		}

		// Mirror the VFS-only host executable to a real versioned path before external launch.
		bool MirrorHostExe()
		{
			const auto mirrorDir = LocalOsfuiDir() / "bin" / kOsfuiReleaseVersion;
			browserHostExeMirror = mirrorDir / "osfui_webview2_host.exe";
			std::error_code ec;
			std::filesystem::create_directories(mirrorDir, ec);
			ec.clear();
			const auto sourceSize = std::filesystem::file_size(browserHostExeSource, ec);
			if (ec) {
				REX::ERROR("WebView2HostWebRenderer: browser-host executable missing at {} ({})",
					browserHostExeSource.string(), ec.message());
				return false;
			}
			ec.clear();
			const auto mirrorSize = std::filesystem::file_size(browserHostExeMirror, ec);
			const bool haveMirror = !ec;
			const bool sameSize = haveMirror && mirrorSize == sourceSize;
			const bool sameTime = haveMirror &&
				std::filesystem::last_write_time(browserHostExeSource, ec) <=
					std::filesystem::last_write_time(browserHostExeMirror, ec);
			if (!(sameSize && sameTime)) {
				ec.clear();
				std::filesystem::copy_file(browserHostExeSource, browserHostExeMirror,
					std::filesystem::copy_options::overwrite_existing, ec);
				if (ec) {
					if (sameSize) {
						// Reuse an in-use mirror only when its content matches the shipped host.
						REX::WARN("WebView2HostWebRenderer: browser-host executable mirror busy; reusing existing copy ({})", ec.message());
					} else {
						REX::ERROR("WebView2HostWebRenderer: browser-host executable mirror copy failed ({})", ec.message());
						return false;
					}
				}
			}
			// Strip the copied Zone.Identifier so a hidden SmartScreen prompt cannot block launch.
			if (HasMarkOfTheWeb(browserHostExeMirror)) {
				if (::DeleteFileW((browserHostExeMirror.native() + L":Zone.Identifier").c_str())) {
					REX::INFO("WebView2HostWebRenderer: stripped Mark-of-the-Web from the "
							  "browser-host executable mirror");
				} else {
					REX::WARN("WebView2HostWebRenderer: browser-host executable mirror carries "
							  "Mark-of-the-Web and stripping it failed ({}) — SmartScreen "
							  "may silently block browser-host launch", ::GetLastError());
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

			// Browser capture textures must use Starfield's adapter.
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
			REX::DEBUG("WebView2HostWebRenderer: starting browser-host transport threads");
			return true;
		}
		// After launch failure, report the failed stage and browser-host log tail.
		void LogBrowserHostStartFailureDiagnostics(std::filesystem::file_time_type a_launchTime)
		{
			std::error_code ec;
			const auto exeSize = std::filesystem::file_size(browserHostExeMirror, ec);
			if (ec) {
				REX::ERROR("BrowserHostDiag: browser-host executable mirror is GONE from {} ({}) — an "
						   "antivirus likely quarantined it; restore/exclude it and retry",
					browserHostExeMirror.string(), ec.message());
			} else if (HasMarkOfTheWeb(browserHostExeMirror)) {
				REX::ERROR("BrowserHostDiag: browser-host executable mirror still carries Mark-of-the-Web — "
						   "SmartScreen has likely blocked the launch silently; unblock {} "
						   "(file Properties -> Unblock)", browserHostExeMirror.string());
			} else {
				REX::INFO("BrowserHostDiag: browser-host executable mirror present ({} bytes, no Mark-of-the-Web)",
					exeSize);
			}

			if (osfui::win32::IsProcessElevated()) {
				REX::ERROR("BrowserHostDiag: the game runs elevated (as administrator) — a "
						   "brokered browser host launches unelevated and cannot open the game "
						   "process; run the game/MO2 without administrator rights");
			}

			REX::INFO("BrowserHostDiag: a browser-host process named osfui_webview2_host.exe {} running",
				BrowserHostProcessRunning() ? "IS still" : "is NOT");

			ec.clear();
			const auto logTime = std::filesystem::last_write_time(browserHostLog, ec);
			if (ec) {
				REX::ERROR("BrowserHostDiag: browser-host log {} does not exist — the browser-host process never "
						   "started (SmartScreen/antivirus block, or the exe failed to run)",
					browserHostLog.string());
				return;
			}
			if (logTime < a_launchTime) {
				REX::ERROR("BrowserHostDiag: browser-host log {} is STALE (predates this launch) — the "
						   "browser-host process never started this session (SmartScreen/antivirus "
						   "block, or the exe failed to run)", browserHostLog.string());
				return;
			}
			const auto tail = ReadLogTail(browserHostLog, 20);
			REX::INFO("BrowserHostDiag: browser-host log tail ({} line(s) from {}):",
				tail.size(), browserHostLog.string());
			for (const auto& line : tail) {
				REX::INFO("BrowserHostDiag: | {}", line);
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
			if (!ResolveMappedViewsRoot()) {
				SignalDead("views cache preparation failed");
				return;
			}
			if (!MirrorHostExe()) {
				SignalDead("browser-host executable preparation failed");
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
				pipeName, ::GetCurrentProcessId(), browserHostLog.native());

			// Claim the first pipe instance before launch to prevent name squatting.
			if (!pipe.CreateServer(pipeName)) {
				SignalDead("could not create the private browser-host pipe: " + pipe.LastErrorText());
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
				browserHostExeMirror.native(), args, /*a_preferBroker=*/usvfs);
			if (!launch.ok) {
				REX::ERROR("WebView2HostWebRenderer: browser-host launch failed [{}]", launch.detail);
				SignalDead("browser-host launch failed");
				return;
			}
			REX::INFO("WebView2HostWebRenderer: browser host launched via {} (usvfs={}, elevated={}){}",
				osfui::wv2::LaunchMethodName(launch.method), usvfs, elevated,
				launch.detail.empty() ? "" : " detail=[" + launch.detail + "]");

			if (!pipe.WaitForClient(20000)) {
				REX::ERROR("WebView2HostWebRenderer: browser host never connected: {} "
						   "(browser-host log: {})", pipe.LastErrorText(), browserHostLog.string());
				LogBrowserHostStartFailureDiagnostics(launchTime);
				SignalDead("browser host did not connect");
				return;
			}
			const auto peerPid = pipe.ClientProcessId();
			if (!peerPid) {
				SignalDead("could not identify the connected browser host: " + pipe.LastErrorText());
				return;
			}

			// Apply one deadline to startup diagnostics and hello.
			const auto helloDeadline = ::GetTickCount64() +
				osfui::wv2::kHelloTimeoutMs;
			std::string payload;
			json hello;
			for (int preHello = 0; ; ++preHello) {
				const auto now = ::GetTickCount64();
				if (now >= helloDeadline ||
					!pipe.ReadMessage(payload, static_cast<std::uint32_t>(
						helloDeadline - now))) {
					REX::ERROR("WebView2HostWebRenderer: browser host connected but did not "
							   "complete hello in {}ms: {}",
						osfui::wv2::kHelloTimeoutMs, pipe.LastErrorText());
					LogBrowserHostStartFailureDiagnostics(launchTime);
					SignalDead("browser-host hello timed out");
					return;
				}
				hello = Json::Parse(payload).value_or(json{});
				if (preHello < 32 && Json::Get(hello, "type", "") == msg::Log::kType) {
					const auto entry = msg::FromJson<msg::Log>(hello);
					if (entry.level >= 2) {
						REX::ERROR("WebView2 browser host: {}", entry.text);
					} else if (entry.level == 1) {
						REX::WARN("WebView2 browser host: {}", entry.text);
					} else {
						REX::INFO("WebView2 browser host: {}", entry.text);
					}
					continue;
				}
				break;
			}

			// Wrong-typed hello fields fall back to defaults and fail this gate.
			const auto greeting = msg::FromJson<msg::Hello>(hello);
			if (Json::Get(hello, "type", "") != msg::Hello::kType ||
				greeting.protocolVersion != osfui::wv2::kBrowserHostProtocolVersion ||
				greeting.pid != *peerPid) {
				REX::ERROR("WebView2HostWebRenderer: rejected browser-host hello "
						   "(protocol={}, claimed pid={}, kernel pid={}, browser-host log: {})",
					greeting.protocolVersion, greeting.pid, *peerPid, browserHostLog.string());
				SignalDead("browser-host identity or protocol mismatch");
				return;
			}

			const HANDLE browserHostProcess = ::OpenProcess(
				SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
				FALSE, *peerPid);
			if (!browserHostProcess) {
				SignalDead(std::format("OpenProcess(browser-host pid {}) failed ({})",
					*peerPid, ::GetLastError()));
				return;
			}
			SetBrowserHostProcess(*peerPid, browserHostProcess);

			const auto webView2RuntimeVersion =
				greeting.runtimeVersion.empty() ? std::string("?") : greeting.runtimeVersion;
			if (webView2RuntimeVersion == "unknown") {
				REX::ERROR("WebView2HostWebRenderer: the WebView2 Evergreen runtime is not "
						   "installed; install it and restart the game");
			}
			REX::INFO("WebView2HostWebRenderer: verified browser-host pid {} up "
					  "(WebView2 Runtime {})", *peerPid, webView2RuntimeVersion);

			using OutItem = osfui::wv2::BoundedQueue<std::string>::Item;
			std::vector<OutItem> bootstrap;
			const auto addBootstrap = [&](json a_message) {
				bootstrap.push_back(OutItem{
					Json::Dump(a_message), {},
					nextOutboundSequence.fetch_add(1, std::memory_order_relaxed) });
			};
			{
				std::scoped_lock lock(stateMutex);
				addBootstrap(ToJson(msg::Init{
					.topLevelHwnd = reinterpret_cast<std::uint64_t>(gameTopLevel),
					.viewsPath = ToUtf8(mappedViewsRoot.native()),
					.width = width,
					.height = height,
					.userDataDir = ToUtf8(userData.native()),
					.devMode = config.devMode,
					.highRefreshCapture = config.highRefreshCapture,
					.hidden = allHidden,
					.adapterLuidLow = adapterLuidLow,
					.adapterLuidHigh = adapterLuidHigh,
				}));
				addBootstrap(ToJson(msg::Viewport{
					.width = viewportWidth,
					.height = viewportHeight,
					.presentationEpoch = presentationEpoch,
				}));
				addBootstrap(ToJson(msg::AccelState{ .toggleScan = accToggle,
					.captured = accCaptured, .captureArmed = accArmed,
					.captureUpScan = accCaptureUp }));
				addBootstrap(ToJson(msg::PointerInput{ .enabled = pointerInputEnabled }));
				accSent = true;
				for (const auto& view : views) {
					addBootstrap(ToJson(msg::Navigate{ .id = view.id, .entry = view.entry,
						.logicalHeight = view.logicalHeight }));
					addBootstrap(ToJson(msg::SetHidden{ .view = view.id,
						.hidden = view.hidden, .presentationEpoch = presentationEpoch }));
					addBootstrap(ToJson(msg::SetOrder{ .view = view.id, .order = view.order }));
				}
				if (!inputTargetId.empty()) {
					addBootstrap(ToJson(msg::SetInputTarget{ .view = inputTargetId }));
				}
				addBootstrap(ToJson(msg::Focus{
					.focused = focusRequested.load(),
					.epoch = focusEpoch.load(),
					.view = inputTargetId,
				}));
				addBootstrap(ToJson(msg::RelativePointerCapture{
					.view = relativePointerView,
					.active = relativePointerActive,
				}));
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
				SignalDead("browser-host connection ended unexpectedly: " +
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
					REX::ERROR("WebView2HostWebRenderer: browser-host heartbeat expired after {}ms",
						osfui::wv2::kHeartbeatTimeoutMs);
					return;
				}
				if (!pipe.ReadMessage(payload, static_cast<std::uint32_t>(
						heartbeatDeadline - now))) {
					return;
				}
				const auto parsed = Json::Parse(payload);
				if (!parsed) continue;
				const json& message = *parsed;
				// Total message decoding prevents malformed page fields from escaping this worker thread.
				const auto type = Json::Get(message, "type", "");
				if (type == msg::Heartbeat::kType) {
					heartbeatDeadline = ::GetTickCount64() +
						osfui::wv2::kHeartbeatTimeoutMs;
					continue;
				}
				if (type == msg::Frame::kType) {
					OnFrameMessage(msg::FromJson<msg::Frame>(message));
				} else if (type == msg::Textures::kType) {
					OnTexturesMessage(msg::FromJson<msg::Textures>(message));
				} else if (type == msg::WebMessage::kType) {
					const auto web = msg::FromJson<msg::WebMessage>(message);
					Push(Notify{ .kind = Notify::Kind::Web,
						.view = web.view, .text = web.json });
				} else if (type == msg::LoadEvent::kType) {
					const auto load = msg::FromJson<msg::LoadEvent>(message);
					Push(Notify{ .kind = Notify::Kind::Load,
						.view = load.view,
						.text = load.url,
						.detail = load.description,
						.failed = load.failed,
						.code = load.code });
				} else if (type == msg::Fatal::kType) {
					const auto fatal = msg::FromJson<msg::Fatal>(message);
					Push(Notify{ .kind = Notify::Kind::Fatal,
						.view = fatal.view,
						.text = fatal.stage,
						.detail = fatal.description,
						.unsignedCode = fatal.code });
				} else if (type == msg::Console::kType) {
					const auto console = msg::FromJson<msg::Console>(message);
					Push(Notify{ .kind = Notify::Kind::Console,
						.view = console.view, .text = console.json });
				} else if (type == msg::Cursor::kType) {
					// Contract allows renderer-thread delivery for cursor.
					if (onCursorChange) {
						onCursorChange(CursorShapeFromSystemCursorId(
							msg::FromJson<msg::Cursor>(message).id));
					}
				} else if (type == msg::Accelerator::kType) {
					// Invoked off the game thread; the handler must stay cheap.
					if (onAccelerator) {
						const auto accel = msg::FromJson<msg::Accelerator>(message);
						onAccelerator(accel.vk, accel.scan, accel.down);
					}
				} else if (type == msg::RelativePointer::kType) {
					// Invoked off the game thread; Runtime only touches atomic accumulators.
					if (onRelativePointer) {
						const auto pointer = msg::FromJson<msg::RelativePointer>(message);
						onRelativePointer(pointer.view, pointer.dx, pointer.dy, pointer.wheel);
					}
				} else if (type == msg::FocusState::kType) {
					const auto state = msg::FromJson<msg::FocusState>(message);
					Push(Notify{ .kind = Notify::Kind::Focus,
						.view = state.view,
						.focused = state.focused,
						.id = state.epoch,
						.sequence = state.sequence });
				} else if (type == msg::Log::kType) {
					const auto entry = msg::FromJson<msg::Log>(message);
					Push(Notify{ .kind = Notify::Kind::Log,
						.text = entry.text, .code = entry.level });
				} else if (type == msg::Ready::kType || type == msg::Hello::kType) {
					// informational
				} else if (type == msg::Bye::kType) {
					REX::INFO("WebView2HostWebRenderer: browser-host bye ({})",
						msg::FromJson<msg::Bye>(message).reason);
				}
			}
		}

		void OnTexturesMessage(const msg::Textures& a_msg)
		{
			// Accept announced ring depths only within the plugin's fixed capacity.
			static_assert(osfui::wv2::kRingSlots <= SharedRingDesc::kMaxSlots);
			SharedRingDesc desc{};
			const auto& slots = a_msg.slots;
			const auto& consumeFences = a_msg.consumeFences;
			const auto closeHandleValue = [](std::uint64_t a_value) {
				if (a_value != 0) {
					::CloseHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(a_value)));
				}
			};
			bool malformed = slots.empty() || slots.size() != consumeFences.size() ||
				a_msg.produceFence == 0;
			for (std::size_t i = 0; !malformed && i < slots.size(); ++i) {
				malformed = slots[i] == 0 || consumeFences[i] == 0;
			}
			if (malformed) {
				for (const auto handle : slots) closeHandleValue(handle);
				for (const auto handle : consumeFences) closeHandleValue(handle);
				closeHandleValue(a_msg.produceFence);
				SignalDead("browser host announced an incomplete shared texture ring");
				return;
			}
			for (std::size_t i = 0; i < SharedRingDesc::kMaxSlots && i < slots.size(); ++i) {
				desc.slotHandles[i] = reinterpret_cast<void*>(
					static_cast<std::uintptr_t>(slots[i]));
				desc.consumeFences[i] = reinterpret_cast<void*>(
					static_cast<std::uintptr_t>(consumeFences[i]));
				++desc.slotCount;
			}
			for (std::size_t i = SharedRingDesc::kMaxSlots; i < slots.size(); ++i) {
				// Close duplicated handles when a mismatched host exceeds ring capacity.
				closeHandleValue(slots[i]);
				closeHandleValue(consumeFences[i]);
			}
			if (slots.size() > SharedRingDesc::kMaxSlots) {
				REX::WARN("WebView2HostWebRenderer: browser host announced {} ring slots, "
						  "capacity is {} — excess slots ignored",
					slots.size(), SharedRingDesc::kMaxSlots);
			}
			// Publish the edge from Update() on the game thread, not this reader thread.
			ringSlotsAnnounced.store(static_cast<std::uint32_t>(slots.size()),
				std::memory_order_relaxed);
			desc.produceFence = reinterpret_cast<void*>(
				static_cast<std::uintptr_t>(a_msg.produceFence));
			desc.width = a_msg.width;
			desc.height = a_msg.height;
			desc.adapterLuidLow = a_msg.adapterLuidLow;
			desc.adapterLuidHigh = a_msg.adapterLuidHigh;
			{
				std::scoped_lock lock(frameMutex);
				desc.generation = ++ringGeneration;
				ringWidth = desc.width;
				ringHeight = desc.height;
				ringSlotCount = desc.slotCount;
				haveFrame = false;  // prior slots are invalid now
			}
			Push(Notify{ .kind = Notify::Kind::Ring, .ring = desc });
		}

		void OnFrameMessage(const msg::Frame& a_msg)
		{
			const auto slot = a_msg.slot;
			const auto serial = a_msg.serial;
			const auto presentation = a_msg.presentationEpoch;
			const auto w = a_msg.width;
			const auto h = a_msg.height;
			std::uint32_t ackSlot = 0;
			std::uint64_t ackSerial = 0;
			bool ackNew = false;
			bool invalidSlot = false;
			{
				std::scoped_lock lock(frameMutex, stateMutex);
				if (slot >= ringSlotCount) {
					invalidSlot = true;
				} else if (w != ringWidth || h != ringHeight) {
					ackNew = true;  // stale ring — release the slot immediately
				} else if (allHidden || presentation != presentationEpoch) {
					// Reject pre-reveal frames and invalidate cached closed-state pixels.
					haveFrame = false;
					ackNew = true;
				} else {
					if (haveFrame && (sharedRingGeneration != submittedRingGeneration || frameSerial != submittedSerial)) {
						// Acknowledge superseded frames that never reached the compositor.
						ackSlot = frameSlot;
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
			if (invalidSlot) {
				SignalDead(std::format("browser host published frame for invalid ring slot {}", slot));
				return;
			}
			if (ackSerial) {
				pipe.WriteMessage(Json::Dump(ToJson(msg::FrameAck{ .slot = ackSlot, .serial = ackSerial })));
			}
			if (ackNew) {
				pipe.WriteMessage(Json::Dump(ToJson(msg::FrameAck{ .slot = slot, .serial = serial })));
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
				REX::WARN("WebView2HostWebRenderer: dropped {} browser-host log line(s) over "
						  "the {}-line pending cap", droppedLogs, kMaxPendingLogs);
			}
			for (auto& value : local) {
				switch (value.kind) {
				case Notify::Kind::Web:
					{
						bool knownView = false;
						{
							std::scoped_lock lock(stateMutex);
							knownView = FindView(value.view) != nullptr;
						}
						// Contain page/handler exceptions at the game-thread drain boundary.
						try {
							if (onWebMessage && knownView) onWebMessage(value.view, value.text);
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
						for (auto* handle : value.ring.consumeFences) {
							if (handle) ::CloseHandle(handle);
						}
					}
					break;
				case Notify::Kind::Log:
					if (value.code >= 2) {
						REX::ERROR("WebView2 browser host: {}", value.text);
					} else if (value.code == 1) {
						REX::WARN("WebView2 browser host: {}", value.text);
					} else {
						REX::INFO("WebView2 browser host: {}", value.text);
					}
					break;
				case Notify::Kind::Focus:
					if (value.sequence > focusAckSequence) {
						focusAckSequence = value.sequence;
						focusAckEpoch = value.id;
						focusActual = value.focused;
						focusActualView = std::move(value.view);
					}
					break;
				case Notify::Kind::Dead:
					if (!deadLogged) {
						deadLogged = true;
						REX::ERROR("WebView2HostWebRenderer: browser-host connection lost — the "
								   "overlay is closing before bounded browser-host recovery begins "
								   "(browser-host log: {})", browserHostLog.string());
						if (onFailure) {
							onFailure(FailureEvent{
								.stage = "host-connection",
								.viewId = inputTargetId,
								.description = "WebView2 browser-host connection lost",
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
			int         level = 0;
			std::string text = a_payload;
			// Parse arbitrary page-authored console payloads leniently without worker-thread escapes.
			if (const auto parsed = Json::Parse(a_payload)) {
				const auto type = Json::Get(*parsed, "type", "log");
				level = type == "warning" ? 1 : type == "error" ? 2 :
					type == "debug" ? 3 : type == "info" ? 4 : 0;
				if (const auto* args = Json::GetArray(*parsed, "args"); args && !args->empty()) {
					const auto& first = args->front();
					text = Json::Get(first, "value",
						Json::Get(first, "description", a_payload));
				}
			}
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
				if (outbound.Push(Json::Dump(ToJson(msg::Shutdown{})), {}, sequence) !=
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
			// Close cancels pipe I/O before joins so the game thread cannot wait indefinitely.
			pipe.Close();
			if (worker.joinable()) worker.join();
			if (writer.joinable()) writer.join();
			connected.store(false, std::memory_order_release);
			{
				std::scoped_lock lock(writerGateMutex);
				writerReady = false;
			}

			if (const HANDLE browserHostProcess = TakeBrowserHostProcess()) {
				const auto graceMs = a_force ? 250u : 3000u;
				if (::WaitForSingleObject(browserHostProcess, graceMs) != WAIT_OBJECT_0) {
					REX::WARN("WebView2HostWebRenderer: verified browser-host pid {} did not exit{}; terminating",
						::GetProcessId(browserHostProcess),
						a_force ? " after transport failure" : " in 3s");
					::TerminateProcess(browserHostProcess, 9);
					::WaitForSingleObject(browserHostProcess, 1000u);
				}
				::CloseHandle(browserHostProcess);
			}

			lifecycle.store(Lifecycle::Stopped, std::memory_order_release);
			// The immutable production generation stays cached. Developer mode owns
			// a mutable per-run mirror and removes it after both host and game leases end.
			{
				std::scoped_lock mirrorLock(viewsMirrorMutex);
				if (viewsCacheLease != INVALID_HANDLE_VALUE) {
					::CloseHandle(viewsCacheLease);
					viewsCacheLease = INVALID_HANDLE_VALUE;
				}
				if (usesViewsMirror && removeViewsMirrorOnStop && mappedViewsRoot != viewsRoot) {
					std::error_code ec;
					std::filesystem::remove_all(mappedViewsRoot, ec);
					if (ec) {
						REX::DEBUG("WebView2HostWebRenderer: developer views mirror cleanup "
								   "deferred to the OS ({})", ec.message());
					}
				}
				usesViewsMirror = false;
				removeViewsMirrorOnStop = false;
			}
		}
		void ResetAfterFailure()
		{
			// Never stall Starfield's main thread waiting for a stranded host after pipe failure.
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
					for (auto* handle : value.ring.consumeFences) {
						if (handle) ::CloseHandle(handle);
					}
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
				submittedRingGeneration = 0;
				submittedSerial = 0;
				ringWidth = ringHeight = 0;
				ringSlotCount = 0;
				announcedGeneration = 0;
				// Keep ring generations monotonic across browser-host processes.
			}
			{
				std::scoped_lock lock(stateMutex);
				accSent = false;
			}

			connected.store(false, std::memory_order_release);
			dead.store(false, std::memory_order_release);
			deadLogged = false;
			focusRequested.store(false);
			focusEpoch.store(0);
			focusAckEpoch = 0;
			focusAckSequence = 0;
			focusActual = false;
			focusActualView.clear();
			focusCheckAccum = 0.0;
			focusMismatchAccum = 0.0;
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

	bool WebView2HostWebRenderer::Initialize(const WebView2HostConfig& a_config)
	{
		// Let the SDK-linked browser host diagnose a missing Evergreen runtime in hello.
		_impl->config = a_config;
		_impl->viewsRoot = a_config.dataDir / "views";
		_impl->userData = LocalOsfuiDir() / "WebView2";
		_impl->browserHostLog = BrowserHostLogPath();
		_impl->browserHostExeSource = a_config.dataDir / "bin" / "osfui_webview2_host.exe";
		_impl->width = (std::max)(1u, a_config.width);
		_impl->height = (std::max)(1u, a_config.height);
		_impl->viewportWidth = _impl->width;
		_impl->viewportHeight = _impl->height;
		std::error_code ec;
		if (!std::filesystem::exists(_impl->browserHostExeSource, ec)) {
			REX::ERROR("WebView2HostWebRenderer: {} is missing — the out-of-process "
					   "browser host was not packaged with this install",
				_impl->browserHostExeSource.string());
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

	void WebView2HostWebRenderer::CreateOrNavigateView(const ViewManifest& a_manifest)
	{
		// Derive stored and sent scale from the same clamp.
		const auto logicalHeight = (std::max)(1u, a_manifest.height);
		{
			std::scoped_lock lock(_impl->stateMutex);
			auto* view = _impl->FindView(a_manifest.id);
			if (!view) {
				view = &_impl->views.emplace_back();
				view->id = a_manifest.id;
			}
			view->entry = a_manifest.entry;
			view->logicalHeight = logicalHeight;
			// Default input to the first instantiated view until runtime policy arrives.
			if (_impl->inputTargetId.empty()) {
				_impl->inputTargetId = a_manifest.id;
			}
		}
		// Re-registering an instantiated view navigates it for dev or crash recovery.
		_impl->Send(ToJson(msg::Navigate{ .id = a_manifest.id, .entry = a_manifest.entry,
			.logicalHeight = logicalHeight }));
	}

    bool WebView2HostWebRenderer::RefreshViewFiles(std::string_view a_viewId)
    {
        return _impl && _impl->RefreshViewFiles(a_viewId);
    }

	void WebView2HostWebRenderer::SetInputTargetView(std::string_view a_id)
	{
		bool changed = false;
		{
			std::scoped_lock lock(_impl->stateMutex);
			if (!_impl->FindView(a_id)) {
				REX::WARN("WebView2HostWebRenderer: SetInputTargetView('{}') ignored — view not instantiated",
					a_id);
				return;
			}
			if (_impl->inputTargetId == a_id) return;
			_impl->inputTargetId = a_id;
			changed = true;
		}
		if (!changed) return;
		_impl->Send(ToJson(msg::SetInputTarget{ .view = std::string(a_id) }));
		if (_impl->focusRequested.load()) {
			const auto epoch = _impl->focusEpoch.fetch_add(1) + 1;
			_impl->focusCheckAccum = 0.0;
			_impl->focusMismatchAccum = 0.0;
			_impl->focusFixWarned = false;
			_impl->Send(ToJson(msg::Focus{
				.focused = true, .epoch = epoch, .view = std::string(a_id) }));
		}
	}

	void WebView2HostWebRenderer::Resize(std::uint32_t a_width, std::uint32_t a_height)
	{
		if (!a_width || !a_height) return;
		{
			std::scoped_lock lock(_impl->stateMutex);
			if (_impl->width == a_width && _impl->height == a_height) return;
			_impl->width = a_width;
			_impl->height = a_height;
		}
		_impl->Send(ToJson(msg::Resize{ .width = a_width, .height = a_height }));
	}

	void WebView2HostWebRenderer::SetViewport(
		const std::uint32_t a_width, const std::uint32_t a_height)
	{
		if (!a_width || !a_height) return;
		std::uint64_t presentation = 0;
		{
			std::scoped_lock lock(_impl->frameMutex, _impl->stateMutex);
			if (_impl->viewportWidth == a_width && _impl->viewportHeight == a_height) return;
			_impl->viewportWidth = a_width;
			_impl->viewportHeight = a_height;
			presentation = ++_impl->presentationEpoch;
			_impl->haveFrame = false;
		}
		_impl->Send(ToJson(msg::Viewport{
			.width = a_width,
			.height = a_height,
			.presentationEpoch = presentation,
		}));
	}

	void WebView2HostWebRenderer::SetPointerInputEnabled(const bool a_enabled)
	{
		{
			std::scoped_lock lock(_impl->stateMutex);
			if (_impl->pointerInputEnabled == a_enabled) return;
			_impl->pointerInputEnabled = a_enabled;
		}
		_impl->Send(ToJson(msg::PointerInput{ .enabled = a_enabled }));
	}

	void WebView2HostWebRenderer::Update(double a_deltaSeconds)
	{
		// Start and initialize the browser host while the overlay remains hidden.
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

		// The host's focus events/acknowledgements are authoritative. Poll Win32 slowly as a safety net for lost OS/WebView events, and only repair a mismatch that persists.
		const auto browserHostSession = _impl->BrowserHostSessionSnapshot();
		if (browserHostSession.topLevel && _impl->connected.load(std::memory_order_acquire)) {
			_impl->focusCheckAccum += a_deltaSeconds;
			if (_impl->focusCheckAccum >= 0.5) {
				const double checkElapsed = _impl->focusCheckAccum;
				_impl->focusCheckAccum = 0.0;
				GUITHREADINFO info{};
				info.cbSize = sizeof(info);
				bool inGameTree = false;
				bool focusInHost = false;
				if (::GetGUIThreadInfo(0, &info) && info.hwndFocus) {
					DWORD focusPid = 0;
					::GetWindowThreadProcessId(info.hwndFocus, &focusPid);
					inGameTree = info.hwndFocus == browserHostSession.topLevel || ::IsChild(browserHostSession.topLevel, info.hwndFocus) != FALSE;
					focusInHost = focusPid != ::GetCurrentProcessId();
				}

				std::string target;
				{
					std::scoped_lock lock(_impl->stateMutex);
					target = _impl->inputTargetId;
				}
				const bool requested = _impl->focusRequested.load();
				const auto epoch = _impl->focusEpoch.load();
				const bool ackMatches = _impl->focusAckEpoch >= epoch && _impl->focusActual == requested && (!requested || target.empty() || _impl->focusActualView == target);
				const bool nativeMatches = inGameTree && (focusInHost == requested);
				const bool healthy = ackMatches && nativeMatches;
				if (healthy || !inGameTree) {
					_impl->focusMismatchAccum = 0.0;
					_impl->focusFixWarned = false;
				} else {
					_impl->focusMismatchAccum += checkElapsed;
					constexpr double kRepairDelaySeconds = 1.0;
					if (_impl->focusMismatchAccum >= kRepairDelaySeconds) {
						_impl->focusMismatchAccum = 0.0;
						if (!_impl->focusFixWarned) {
							_impl->focusFixWarned = true;
							REX::WARN("WebView2HostWebRenderer: focus mismatch persisted for {:.1f}s (desired={} target='{}' epoch={}, actual={} view='{}' ackEpoch={}); applying safety repair", 
								kRepairDelaySeconds, requested, target, epoch, _impl->focusActual, _impl->focusActualView, _impl->focusAckEpoch);
						}
						if (requested) {
							_impl->Send(ToJson(msg::SetInputTarget{ .view = target }));
							_impl->Send(ToJson(msg::Focus{ .focused = true, .epoch = epoch, .view = target }));
						} else {
							::PostMessageW(browserHostSession.topLevel, OverlayInputHook::kRestoreGameFocusMessage, static_cast<WPARAM>(epoch), 0);
						}
					}
				}
			}
		} else {
			_impl->focusCheckAccum = 0.0;
			_impl->focusMismatchAccum = 0.0;
			_impl->focusFixWarned = false;
		}

		// Report truncated ring depth as a game-thread degradation, not total failure.
		if (const auto announced = _impl->ringSlotsAnnounced.exchange(0, std::memory_order_relaxed);
			announced != 0 && announced != _impl->ringSlotsReported) {
			_impl->ringSlotsReported = announced;
			const bool truncated = announced > SharedRingDesc::kMaxSlots;
			const auto detail = truncated ?
				std::format("browser host announced {} slots, capacity {}",
					announced, SharedRingDesc::kMaxSlots) :
				std::string{};
			_impl->ReportHealth("host.ring-truncated", truncated, detail);
		}
	}

	std::optional<FrameBufferView> WebView2HostWebRenderer::TakeLatestFrame()
	{
		std::scoped_lock lock(_impl->frameMutex);
		if (!_impl->haveFrame ||
			_impl->sharedRingGeneration != _impl->announcedGeneration) {
			// Wait until Update announces the frame's ring to the compositor.
			return std::nullopt;
		}
		if (_impl->submittedRingGeneration == _impl->sharedRingGeneration && _impl->submittedSerial == _impl->frameSerial) {
			return std::nullopt;
		}
		_impl->submittedRingGeneration = _impl->sharedRingGeneration;
		_impl->submittedSerial = _impl->frameSerial;
		return FrameBufferView{
			.width = _impl->frameWidth,
			.height = _impl->frameHeight,
			.ringGeneration = _impl->sharedRingGeneration,
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
			if (!view) return;
		}
		_impl->SendOrQueue(ToJson(msg::PostWeb{ .view = std::string(a_viewId),
			.json = std::string(a_json) }));
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
	void WebView2HostWebRenderer::SetRelativePointerHandler(RelativePointerHandler a_handler)
	{
		_impl->onRelativePointer = std::move(a_handler);
	}
	void WebView2HostWebRenderer::SetRelativePointerCapture(
		std::string_view a_viewId, bool a_active)
	{
		{
			std::scoped_lock lock(_impl->stateMutex);
			_impl->relativePointerView = a_active ? std::string(a_viewId) : std::string{};
			_impl->relativePointerActive = a_active;
		}
		_impl->Send(ToJson(msg::RelativePointerCapture{
			.view = a_active ? std::string(a_viewId) : std::string{},
			.active = a_active,
		}));
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
		_impl->focusRequested.store(a_focused);
		const auto epoch = _impl->focusEpoch.fetch_add(1) + 1;
		std::string target;
		{
			std::scoped_lock lock(_impl->stateMutex);
			target = _impl->inputTargetId;
		}
		_impl->focusCheckAccum = 0.0;
		_impl->focusMismatchAccum = 0.0;
		_impl->focusFixWarned = false;
		if (a_focused) {
			_impl->Start();
		}
		_impl->Send(ToJson(msg::Focus{ .focused = a_focused, .epoch = epoch, .view = target }));
		const auto browserHostSession = _impl->BrowserHostSessionSnapshot();
		if (!a_focused && browserHostSession.topLevel) {
			// Restore game focus on the game's own window thread.
			::PostMessageW(browserHostSession.topLevel, OverlayInputHook::kRestoreGameFocusMessage, static_cast<WPARAM>(epoch), 0);
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
			_impl->Send(ToJson(msg::AccelState{ .toggleScan = a_toggleScan,
				.captured = a_captured, .captureArmed = a_captureArmed,
				.captureUpScan = a_captureUpScan }));
		}
	}

	void WebView2HostWebRenderer::InjectKeyEvent(std::uint32_t a_vkCode, bool a_down)
	{
		// Synthetic page keys cover gamepad navigation and Esc; physical keyboard and IME stay native.
		_impl->Send(ToJson(msg::Key{ .vk = a_vkCode, .down = a_down }));
	}

	void WebView2HostWebRenderer::InjectMouseMove(int a_x, int a_y)
	{
		_impl->Send(ToJson(msg::Mouse{ .kind = "move", .x = a_x, .y = a_y }));
	}
	void WebView2HostWebRenderer::InjectMouseButton(
		int a_x, int a_y, int a_button, bool a_down)
	{
		_impl->Send(ToJson(msg::Mouse{ .kind = "button", .x = a_x, .y = a_y,
			.button = a_button, .down = a_down }));
	}
	void WebView2HostWebRenderer::InjectMouseWheel(int a_x, int a_y, int a_wheelDelta)
	{
		_impl->Send(ToJson(msg::Mouse{ .kind = "wheel", .x = a_x, .y = a_y,
			.wheel = a_wheelDelta }));
	}

	void WebView2HostWebRenderer::InjectPhysicalMouseWheel(
		int a_x, int a_y, int a_wheelDelta)
	{
		_impl->Send(ToJson(msg::Mouse{ .kind = "physicalWheel", .x = a_x, .y = a_y,
			.wheel = a_wheelDelta }));
	}

	void WebView2HostWebRenderer::OpenDevTools(std::string_view a_viewId)
	{
		_impl->Send(ToJson(msg::OpenDevTools{ .view = std::string(a_viewId) }));
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
			if (view->hidden == a_hidden) return;
			const bool wasHidden = view->hidden;
			view->hidden = a_hidden;
			_impl->RecomputeAllHidden();
			if (wasHidden && !a_hidden) {
				// Every newly shown view is a new presentation, including menu-to-menu switches where another view kept the overlay visible.
				++_impl->presentationEpoch;
				_impl->haveFrame = false;
			} else if (_impl->allHidden) {
				_impl->haveFrame = false;
			}
			presentation = _impl->presentationEpoch;
		}
		_impl->Send(ToJson(msg::SetHidden{ .view = std::string(a_viewId),
			.hidden = a_hidden, .presentationEpoch = presentation }));
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
		_impl->Send(ToJson(msg::SetOrder{ .view = std::string(a_viewId), .order = a_order }));
	}

	void WebView2HostWebRenderer::DestroyView(std::string_view a_viewId)
	{
		{
			std::scoped_lock lock(_impl->stateMutex);
			const auto* view = _impl->FindView(a_viewId);
			if (!view) return;
			std::erase_if(_impl->views,
				[&](const Impl::ViewRec& a_rec) { return a_rec.id == a_viewId; });
			if (_impl->inputTargetId == a_viewId) {
				_impl->inputTargetId = _impl->views.empty() ? std::string{} : _impl->views.front().id;
			}
			_impl->RecomputeAllHidden();
		}
		// Game-thread map; no lock.
		_impl->consoleHandlers.erase(std::string(a_viewId));
		_impl->Send(ToJson(msg::DestroyView{ .view = std::string(a_viewId) }));
	}
}
