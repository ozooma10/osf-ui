#include "Render/BrowserHostClient.h"
#include "Render/SharedFrameConsumer.h"
#include "Render/BrowserHostRuntime.h"

#include <atomic>
#include <deque>
#include <future>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Composite/EngineD3D12.h"
#include "Core/Log.h"
#include "Core/Utf8Path.h"
#include "Core/Version.h"
#include "Views/Dev/DevViewFiles.h"
#include "Views/ViewCache.h"
#include "Core/Json.h"
#include "Input/BrowserKeyboard.h"
#include "Input/OverlayInputHook.h"

#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>
#include <ShlObj.h>
#include <TlHelp32.h>
#include <d3d12.h>
#include <nlohmann/json.hpp>

#include "BrowserBoundedQueue.h"
#include "BrowserBrokerLaunch.h"
#include "BrowserMessages.h"
#include "BrowserPipe.h"
#include "BrowserProtocol.h"
#include "Win32Util.h"

using nlohmann::json;


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


		bool HasMarkOfTheWeb(const std::filesystem::path& a_file)
		{
			return ::GetFileAttributesW((a_file.native() + L":Zone.Identifier").c_str()) !=
			       INVALID_FILE_ATTRIBUTES;
		}

		bool BrowserHostProcessRunning(const std::wstring& a_name)
		{
			const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
			if (snapshot == INVALID_HANDLE_VALUE) {
				return false;
			}
			PROCESSENTRY32W entry{ .dwSize = sizeof(PROCESSENTRY32W) };
			bool found = false;
			if (::Process32FirstW(snapshot, &entry)) {
				do {
					if (::_wcsicmp(entry.szExeFile, a_name.c_str()) == 0) {
						found = true;
						break;
					}
				} while (::Process32NextW(snapshot, &entry));
			}
			::CloseHandle(snapshot);
			return found;
		}

		void RemoveLegacyMirrors(const std::filesystem::path& a_localRoot)
		{
			std::vector<std::filesystem::path> legacy;
			std::error_code ec;
			for (std::filesystem::directory_iterator it(a_localRoot, ec), end; !ec && it != end; it.increment(ec)) {
				const auto name = Utf8Path(it->path().filename());
				if (name == "views-mirror" || name.starts_with("views-mirror-") || name.starts_with("views-dev-")) {
					legacy.push_back(it->path());
				}
			}
			for (std::filesystem::directory_iterator it(a_localRoot / "bin", ec), end; !ec && it != end; it.increment(ec)) {
				if (it->is_directory(ec)) legacy.push_back(it->path());
			}
			std::size_t removed = 0;
			for (const auto& path : legacy) {
				std::error_code removeEc;
				std::filesystem::remove_all(path, removeEc);
				if (removeEc) {
					REX::DEBUG("BrowserHostClient: legacy mirror '{}' not removed ({})", Utf8Path(path), removeEc.message());
				} else {
					++removed;
				}
			}
			if (removed) {
				REX::INFO("BrowserHostClient: removed {} legacy mirror folder(s)", removed);
			}
		}

		std::optional<std::filesystem::path> PrepareModCaches(const std::filesystem::path& a_source,
			const std::filesystem::path& a_cacheRoot, std::string_view a_label)
		{
			const auto started = std::chrono::steady_clock::now();
			std::string error;
			const auto prepared = ViewCache::PrepareMods(a_source, a_cacheRoot, kOsfuiReleaseVersion,
				std::format("{}-{}", ::GetCurrentProcessId(), ::GetTickCount64()), error);
			if (!prepared) {
				REX::ERROR("BrowserHostClient: {} preparation failed ({})", a_label, error);
				return std::nullopt;
			}
			std::size_t reused = 0;
			std::uint64_t copiedBytes = 0;
			for (const auto& entry : prepared->entries) {
				const auto& cache = entry.cache;
				if (cache.reused) ++reused;
				else copiedBytes += cache.fingerprint.bytes;
				REX::INFO("BrowserHostClient: USVFS {} '{}' {} ({} files, {:.2f} MiB; scan={} ms, copy={} ms, verify={} ms)",
					a_label, entry.name, cache.reused ? "reused" : "published", cache.fingerprint.files,
					static_cast<double>(cache.fingerprint.bytes) / (1024.0 * 1024.0), cache.scanMs, cache.copyMs, cache.verifyMs);
			}
			const auto cleanupStarted = std::chrono::steady_clock::now();
			const auto scavenged = ViewCache::Scavenge(a_cacheRoot, prepared->root);
			const auto cleanupMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - cleanupStarted).count();
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
			REX::INFO("BrowserHostClient: USVFS {} ready {} ({} reused, {} rebuilt, {:.2f} MiB copied, {} ms; cleanup={} ms, {} obsolete entries, {} old cache(s) removed)",
				a_label, ToUtf8(prepared->root.native()), reused, prepared->entries.size() - reused,
				static_cast<double>(copiedBytes) / (1024.0 * 1024.0), elapsed, cleanupMs, prepared->removed, scavenged.removed);
			if (scavenged.failed + prepared->failed) {
				REX::WARN("BrowserHostClient: {} {} item(s) could not be scavenged; they will be retried next launch",
					scavenged.failed + prepared->failed, a_label);
			}
			return prepared->root;
		}

		// Removes a copy this session no longer uses (MO2 left, dev mode off, legacy views gone).
		void RemoveUnusedCopy(const std::filesystem::path& a_path)
		{
			std::error_code ec;
			if (!std::filesystem::exists(a_path, ec)) return;
			ViewCache::RemoveTree(a_path, ec);
			if (ec) REX::WARN("BrowserHostClient: unused copy '{}' not removed ({}); retrying next launch", Utf8Path(a_path), ec.message());
			else REX::INFO("BrowserHostClient: removed unused copy '{}'", Utf8Path(a_path));
		}

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

		// Host log levels: 0 info, 1 warning, 2+ error.
		void LogBrowserHostLine(int a_level, const std::string& a_text)
		{
			if (a_level >= 2) {
				REX::ERROR("OSF UI browser host: {}", a_text);
			} else if (a_level == 1) {
				REX::WARN("OSF UI browser host: {}", a_text);
			} else {
				REX::INFO("OSF UI browser host: {}", a_text);
			}
		}

		// Copy values while holding stateMutex; do not keep ViewRec references or string_views after unlocking.
		namespace msg = osfui::browser::msg;
	}

	struct BrowserHostClient::Impl
	{
		struct Notify
		{
			enum class Kind { Web, Load, Fatal, Console, Log, Dead, Back };
			Kind           kind{ Kind::Web };
			std::string    view;
			std::string    text, detail;
			bool           failed{};
			int            code{};
			std::uint32_t  unsignedCode{};
			std::uint64_t  presentationEpoch{};
		};

		BrowserHostConfig    config;
		std::string          language;  // Resolved before Start; immutable while the worker runs.
		std::filesystem::path viewsRoot, mappedViewsRoot, legacyViewsRoot, mappedLegacyViewsRoot, userData;
        // Guards writes to the real-path mirror.
        std::mutex            viewsMirrorMutex;
		bool                  usesDevViewsMirror{ false };  // mutable views-dev tree
		std::shared_future<bool> viewFilesPreparation;  // Started by Runtime; consumed by the transport worker.
		std::filesystem::path browserHostExeSource, browserHostExeMirror;
		std::filesystem::path browserBrokerExe;
		bool                  runtimeCopiesPruned{ false };  // worker-only; once per session
		std::filesystem::path browserHostLog;  // set in Initialize; read by worker + notify drain
		std::uint32_t adapterLuidLow{ 0 }, adapterLuidHigh{ 0 };
		bool          adapterLuidKnown{ false };

		WebMessageHandler       onWebMessage;
		LoadHandler             onLoad;
		FailureHandler          onFailure;
		CursorChangeHandler     onCursorChange;
		BackHandler             onBack;
		// Game-thread only (Drain/setters).
		std::unordered_map<std::string, ConsoleHandler>    consoleHandlers;  // viewId -> cb

		// Preserve creation order for equal z values; send later changes after the worker's snapshot.
		struct ViewRec
		{
			std::string id;
			std::string entry;
			bool legacy{};
			bool        hidden{ true };
			bool settleColdOpen{ false };
			int         order{ 0 };
			// Scale the page's CSS height to the output height.
			std::uint32_t logicalHeight{ kDefaultViewHeight };
		};
		std::mutex           stateMutex;
		std::vector<ViewRec> views;
		std::string          inputTargetId;
		bool                 allHidden{ true };  // no visible view => incoming frames are released
		// Each show needs a new frame before becoming visible.
		std::uint64_t        presentationEpoch{ 0 };
		std::uint32_t        width{ 1 }, height{ 1 };
		std::uint32_t        viewportWidth{ 1 }, viewportHeight{ 1 };
		bool                 pointerInputEnabled{ true };
		bool                 focusRequested{ false };
		bool                 windowActive{ true };  // activation messages, reconciled against the foreground window each tick

		// Failed persists until RestartAfterFailure.
		enum class Lifecycle : std::uint8_t
		{
			Stopped,
			Starting,
			Running,
			Stopping,
			Failed
		};

		osfui::browser::Pipe pipe;
		std::thread      worker;
		std::thread      writer;  // Spawned by the worker once connected; joined after the worker.
		std::atomic<Lifecycle> lifecycle{ Lifecycle::Stopped };
		HANDLE hostProcess{ nullptr };  // Written by the worker; Stop reads it only after joining the worker.

		std::mutex         notifyMutex;
		std::deque<Notify> notifications;  // unbounded: the host rate-limits page messages and forwards console only in dev mode

		std::shared_ptr<SharedFrameConsumer> frames{ std::make_shared<SharedFrameConsumer>() };

		void Push(Notify a_value)
		{
			std::scoped_lock lock(notifyMutex);
			notifications.push_back(std::move(a_value));
		}

		static constexpr std::size_t kMaxOutbound = 512;
		osfui::browser::BoundedQueue<std::string> outbound{ kMaxOutbound };

		bool IsRunning() const { return lifecycle.load(std::memory_order_acquire) == Lifecycle::Running; }
		bool IsStarting() const { return lifecycle.load(std::memory_order_acquire) == Lifecycle::Starting; }

		void SignalDead(std::string_view a_reason)
		{
			// Ignore failures after stopping or an earlier failure.
			auto prior = lifecycle.load(std::memory_order_acquire);
			bool failed = false;
			while (prior == Lifecycle::Starting || prior == Lifecycle::Running) {
				if (lifecycle.compare_exchange_weak(prior, Lifecycle::Failed, std::memory_order_acq_rel)) {
					failed = true;
					break;
				}
			}
			if (failed) {
				REX::ERROR("BrowserHostClient: {}", a_reason);
				Push(Notify{ .kind = Notify::Kind::Dead });
			}
			outbound.Close();
			pipe.Close();
		}

		std::string CoalesceKey(const json& a_msg)
		{
			return osfui::browser::GameMessageCoalesceKey(
				Json::Get(a_msg, "type", ""),
				Json::Get(a_msg, "kind", ""),
				Json::Get(a_msg, "view", ""));
		}

		bool Enqueue(const json& a_msg, bool a_queueBeforeConnect)
		{
			if (!a_queueBeforeConnect && !IsRunning()) {
				return false;
			}
			const auto result = outbound.Push(Json::Dump(a_msg), CoalesceKey(a_msg));
			if (result == decltype(outbound)::PushResult::Full) {
				SignalDead(std::format(
					"outbound IPC message queue exceeded {} messages", kMaxOutbound));
				return false;
			}
			return result != decltype(outbound)::PushResult::Closed;
		}

		void Send(const json& a_msg) { Enqueue(a_msg, false); }

		// Queue the connection snapshot before any events.
		void SendOrQueue(const json& a_msg) { Enqueue(a_msg, true); }

		bool PublishConnected(std::vector<osfui::browser::BoundedQueue<std::string>::Item> a_bootstrap)
		{
			// Prepend before Running: a Send that observes Running is ordered after the snapshot.
			// Stop or failure can cancel startup and close the queue.
			if (!outbound.Prepend(std::move(a_bootstrap))) return false;
			auto expected = Lifecycle::Starting;
			return lifecycle.compare_exchange_strong(expected, Lifecycle::Running, std::memory_order_acq_rel);
		}

		void WriterMain()
		{
			decltype(outbound)::Item item;
			while (outbound.WaitPop(item)) {
				const auto result = pipe.WriteMessage(item.value);
				if (result == osfui::browser::Pipe::WriteResult::InvalidPayload) {
					REX::WARN("BrowserHostClient: dropped oversized outbound message");
				}
				if (result == osfui::browser::Pipe::WriteResult::Disconnected) {
					SignalDead("outbound pipe writer failed: " + pipe.LastErrorText());
					return;
				}
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

		// Requires stateMutex. Reject frames from earlier presentations.
		void BeginPresentation()
		{
			frames->SetPresentation(++presentationEpoch, !allHidden);
		}

		void SendViewport()
		{
			Send(ToJson(msg::Viewport{ .width = viewportWidth, .height = viewportHeight, .presentationEpoch = presentationEpoch }));
		}

		// Startup (worker thread)

		// Copy views outside MO2's USVFS for the browser. Hot reload uses a separate copy.
		void PrepareViewFiles()
		{
			if (viewFilesPreparation.valid()) return;
			REX::INFO("BrowserHostClient: preparing view files in the background");
			viewFilesPreparation = std::async(std::launch::async, [this] {
				try {
					const bool ready = ResolveMappedViewsRoot();
					if (ready) REX::INFO("BrowserHostClient: view files ready for this session");
					return ready;
				} catch (const std::exception& error) {
					REX::ERROR("BrowserHostClient: view file preparation failed ({})", error.what());
					return false;
				}
			}).share();
		}

		bool ResolveMappedViewsRoot()
		{
			std::scoped_lock mirrorLock(viewsMirrorMutex);
			mappedViewsRoot = viewsRoot;
			mappedLegacyViewsRoot = legacyViewsRoot;
			usesDevViewsMirror = false;

			const auto localRoot = LocalOsfuiDir();
			RemoveLegacyMirrors(localRoot);
			const auto legacyCache = localRoot / "legacy-views-cache";
			const auto modernCache = localRoot / ViewCache::kCacheDirectory;
			const auto devMirror = localRoot / "views-dev";
			if (!::GetModuleHandleW(L"usvfs_x64.dll")) {
				// Real installs serve views in place; copies from earlier MO2 sessions are unused.
				for (const auto& copy : { legacyCache, modernCache, devMirror }) RemoveUnusedCopy(copy);
				return true;
			}

			std::error_code legacyError;
			if (std::filesystem::is_directory(legacyViewsRoot, legacyError)) {
				const auto legacy = PrepareModCaches(legacyViewsRoot, legacyCache, "legacy views cache");
				if (!legacy) return false;
				mappedLegacyViewsRoot = *legacy;
			} else {
				RemoveUnusedCopy(legacyCache);
			}
			if (config.devMode) {
				std::string error;
				if (!DevViewFiles::ReplaceTree(viewsRoot, devMirror, error)) {
					REX::ERROR("BrowserHostClient: developer views mirror failed ({})", error);
					return false;
				}
				mappedViewsRoot = devMirror;
				usesDevViewsMirror = true;
				REX::INFO("BrowserHostClient: USVFS developer views mirrored to {}", ToUtf8(devMirror.native()));
				return true;  // The normal views cache stays for reuse when dev mode is turned off.
			}

			RemoveUnusedCopy(devMirror);
			const auto modern = PrepareModCaches(viewsRoot, modernCache, "views cache");
			if (!modern) return false;
			mappedViewsRoot = *modern;
			return true;
		}

        // Refresh the browser's file copy before navigation.
		bool RefreshModFiles(std::string_view a_mod)
		{
			std::scoped_lock mirrorLock(viewsMirrorMutex);
			if (!usesDevViewsMirror) return true;

			// Copy the whole mod folder because views can share assets.
			const auto modFolder = std::filesystem::path(a_mod);
			const auto source = viewsRoot / modFolder;
			const auto destination = mappedViewsRoot / modFolder;
			std::string error;
			if (!DevViewFiles::ReplaceTree(source, destination, error)) {
				REX::WARN("BrowserHostClient: dev reload could not mirror '{}' ({})", a_mod, error);
				return false;
			}
			return true;
		}

        bool MirrorHostExe()
        {
            const auto prepared = PrepareHostExecutable(config.backend, browserHostExeSource, LocalOsfuiDir());
            if (!prepared) return false;
            browserHostExeMirror = *prepared;
            const auto broker = PrepareBrowserBroker(config.dataDir, LocalOsfuiDir());
            if (!broker) return false;
            browserBrokerExe = *broker;
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
				REX::DEBUG("BrowserHostClient: game adapter LUID 0x{:08X}:0x{:08X}",
					adapterLuidHigh, adapterLuidLow);
			}

			pipe.PrepareForOpen();
			PrepareViewFiles();  // Also covers view demand before the data-load notification.
			worker = std::thread([this] { WorkerMain(); });
			REX::DEBUG("BrowserHostClient: starting browser-host transport threads");
			return true;
		}
		void LogBrowserHostStartFailureDiagnostics(std::filesystem::file_time_type a_launchTime)
		{
			std::error_code ec;
			const auto exeSize = std::filesystem::file_size(browserHostExeMirror, ec);
			if (ec) {
				REX::ERROR("BrowserHostDiag: browser-host executable mirror is GONE from {} ({}) — an "
						   "antivirus likely quarantined it; restore/exclude it and retry",
					Utf8Path(browserHostExeMirror), ec.message());
			} else if (HasMarkOfTheWeb(browserHostExeMirror)) {
				REX::ERROR("BrowserHostDiag: browser-host executable mirror still carries Mark-of-the-Web — "
						   "SmartScreen has likely blocked the launch silently; unblock {} "
						   "(file Properties -> Unblock)", Utf8Path(browserHostExeMirror));
			} else {
				REX::INFO("BrowserHostDiag: browser-host executable mirror present ({} bytes, no Mark-of-the-Web)",
					exeSize);
			}

			if (osfui::win32::IsProcessElevated()) {
				REX::ERROR("BrowserHostDiag: the game runs elevated (as administrator) — a "
						   "brokered browser host launches unelevated and cannot open the game "
						   "process; run the game/MO2 without administrator rights");
			}

			REX::INFO("BrowserHostDiag: a browser-host process named {} {} running",
				Utf8Path(browserHostExeSource.filename()),
				BrowserHostProcessRunning(browserHostExeSource.filename().native()) ? "IS still" : "is NOT");

			ec.clear();
			const auto logTime = std::filesystem::last_write_time(browserHostLog, ec);
			if (ec) {
				REX::ERROR("BrowserHostDiag: browser-host log {} does not exist — the browser-host process never "
						   "started (SmartScreen/antivirus block, or the exe failed to run)",
					Utf8Path(browserHostLog));
				return;
			}
			if (logTime < a_launchTime) {
				REX::ERROR("BrowserHostDiag: browser-host log {} is STALE (predates this launch) — the "
						   "browser-host process never started this session (SmartScreen/antivirus "
						   "block, or the exe failed to run)", Utf8Path(browserHostLog));
				return;
			}
			const auto tail = ReadLogTail(browserHostLog, 20);
			REX::INFO("BrowserHostDiag: browser-host log tail ({} line(s) from {}):",
					tail.size(), Utf8Path(browserHostLog));
			for (const auto& line : tail) {
				REX::INFO("BrowserHostDiag: | {}", line);
			}
		}

		void WorkerMain()
		{
			if (!viewFilesPreparation.get()) {
				SignalDead("views cache preparation failed");
				return;
			}
			if (lifecycle.load(std::memory_order_acquire) != Lifecycle::Starting) return;
			if (!MirrorHostExe()) {
				SignalDead("browser-host executable preparation failed");
				return;
			}

			auto pipeSeed = ::GetTickCount64() ^
				(static_cast<std::uint64_t>(::GetCurrentProcessId()) << 17);
			std::mt19937_64 rng(pipeSeed);
			const auto nonce = static_cast<std::uint32_t>(rng());
			const auto pipeName = std::format(L"{}{}-{:08x}",
				osfui::browser::kPipePrefix, ::GetCurrentProcessId(), nonce);
			auto args = std::format(L"--pipe={} --game-pid={} --log=\"{}\"",
				pipeName, ::GetCurrentProcessId(), browserHostLog.native());

			// Claim the first pipe instance before launch to prevent name squatting.
			if (!pipe.CreateServer(pipeName)) {
				SignalDead("could not create the private browser-host pipe: " + pipe.LastErrorText());
				return;
			}
			if (!IsStarting()) return;

			const bool usvfs = ::GetModuleHandleW(L"usvfs_x64.dll") != nullptr;
			const bool elevated = osfui::win32::IsProcessElevated();
			if (elevated && usvfs) {
				REX::WARN("BrowserHostClient: the game is running elevated under "
						  "MO2; using the elevated broker fallback");
			}
			const auto launchTime = std::filesystem::file_time_type::clock::now();
			const auto launch = osfui::browser::LaunchDetached(
				browserHostExeMirror.native(), args, browserBrokerExe.native(), /*a_preferBroker=*/usvfs, [this] { return !IsStarting(); });
			if (!launch.ok) {
				REX::ERROR("BrowserHostClient: browser-host launch failed [{}]", launch.detail);
				SignalDead("browser-host launch failed");
				return;
			}
			REX::INFO("BrowserHostClient: browser host launched via {} (usvfs={}, elevated={}){}",
				osfui::browser::LaunchMethodName(launch.method), usvfs, elevated,
				launch.detail.empty() ? "" : " detail=[" + launch.detail + "]");

			if (!pipe.WaitForClient(20000)) {
				REX::ERROR("BrowserHostClient: browser host never connected: {} "
						   "(browser-host log: {})", pipe.LastErrorText(), Utf8Path(browserHostLog));
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
				osfui::browser::kHelloTimeoutMs;
			std::string payload;
			json hello;
			for (int preHello = 0; ; ++preHello) {
				const auto now = ::GetTickCount64();
				if (now >= helloDeadline ||
					!pipe.ReadMessage(payload, static_cast<std::uint32_t>(
						helloDeadline - now))) {
					REX::ERROR("BrowserHostClient: browser host connected but did not complete hello in {}ms: {}",
						osfui::browser::kHelloTimeoutMs, pipe.LastErrorText());
					LogBrowserHostStartFailureDiagnostics(launchTime);
					SignalDead("browser-host hello timed out");
					return;
				}
				hello = Json::Parse(payload).value_or(json{});
				if (preHello < 32 && Json::Get(hello, "type", "") == msg::Log::kType) {
					const auto entry = msg::FromJson<msg::Log>(hello);
					LogBrowserHostLine(entry.level, entry.text);
					continue;
				}
				break;
			}

			const auto greeting = msg::FromJson<msg::Hello>(hello);
			if (Json::Get(hello, "type", "") != msg::Hello::kType ||
				greeting.protocolVersion != osfui::browser::kBrowserHostProtocolVersion) {
				REX::ERROR("BrowserHostClient: rejected browser-host hello (protocol={}, pid={}, browser-host log: {})",
					greeting.protocolVersion, *peerPid, Utf8Path(browserHostLog));
				SignalDead("browser-host protocol mismatch");
				return;
			}

			const HANDLE browserHostProcess = ::OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *peerPid);
			if (!browserHostProcess) {
				SignalDead(std::format("OpenProcess(browser-host pid {}) failed ({})", *peerPid, ::GetLastError()));
				return;
			}
			hostProcess = browserHostProcess;

			const auto runtimeVersion = greeting.runtimeVersion.empty() ? std::string("?") : greeting.runtimeVersion;
            if (!msg::IsCompatibleHello(greeting, config.backend)) {
                SignalDead("browser-host backend or capabilities do not match the selected edition");
                return;
            }
            REX::INFO("OSF UI: verified {} browser-host pid {} up (runtime {})",
                osfui::browser::BackendName(config.backend), *peerPid, runtimeVersion);
            if (!std::exchange(runtimeCopiesPruned, true)) {
                // Deleting an old ~400 MiB runtime must not delay the first frame.
                std::thread([root = LocalOsfuiDir(), host = browserHostExeMirror, broker = browserBrokerExe] {
                    PruneBrowserRuntimeCopies(root, host, broker);
                }).detach();
            }

			using OutItem = osfui::browser::BoundedQueue<std::string>::Item;
			std::vector<OutItem> bootstrap;
			const auto addBootstrap = [&](json a_message) {
				bootstrap.push_back(OutItem{ Json::Dump(a_message) });
			};
			{
				std::scoped_lock lock(stateMutex);
				addBootstrap(ToJson(msg::Init{
					.topLevelHwnd = reinterpret_cast<std::uint64_t>(OverlayInputHook::GameWindow()),
					.viewsPath = ToUtf8(mappedViewsRoot.native()),
					.legacyViewsPath = ToUtf8(mappedLegacyViewsRoot.native()),
					.width = width,
					.height = height,
					.userDataDir = ToUtf8(userData.native()),
					.devMode = config.devMode,
					.language = language,
					.adapterLuidLow = adapterLuidLow,
					.adapterLuidHigh = adapterLuidHigh,
				}));
				addBootstrap(ToJson(msg::Viewport{
					.width = viewportWidth,
					.height = viewportHeight,
					.presentationEpoch = presentationEpoch,
				}));
				addBootstrap(ToJson(msg::PointerInput{ .enabled = pointerInputEnabled }));
				addBootstrap(ToJson(msg::WindowActive{ .active = windowActive }));
				for (const auto& view : views) {
					addBootstrap(ToJson(msg::Navigate{ .id = view.id, .entry = view.entry,
						.logicalHeight = view.logicalHeight, .legacy = view.legacy }));
					addBootstrap(ToJson(msg::SetHidden{ .view = view.id,
						.hidden = view.hidden, .presentationEpoch = presentationEpoch, .settleColdOpen = view.settleColdOpen }));
					addBootstrap(ToJson(msg::SetOrder{ .view = view.id, .order = view.order }));
				}
				if (!inputTargetId.empty()) {
					addBootstrap(ToJson(msg::SetInputTarget{ .view = inputTargetId }));
				}
				addBootstrap(ToJson(msg::Focus{ .focused = focusRequested }));
				// Hold stateMutex so the snapshot is queued before later changes.
				if (!PublishConnected(std::move(bootstrap))) {
					SignalDead("connection snapshot exceeded the outbound queue limit");
					return;
				}
			}

			// Stop joins the worker before the writer, so this assignment is visible to it.
			writer = std::thread([this] { WriterMain(); });
			ReadLoop();
			SignalDead("browser-host connection ended unexpectedly: " + pipe.LastErrorText());
		}
		// Inbound (worker thread)

		void ReadLoop()
		{
			std::string payload;
			auto heartbeatDeadline = ::GetTickCount64() +
				osfui::browser::kHeartbeatTimeoutMs;
			while (IsRunning()) {
				const auto now = ::GetTickCount64();
				if (now >= heartbeatDeadline) {
					REX::ERROR("BrowserHostClient: browser-host heartbeat expired after {}ms",
						osfui::browser::kHeartbeatTimeoutMs);
					return;
				}
				if (!pipe.ReadMessage(payload, static_cast<std::uint32_t>(
						heartbeatDeadline - now))) {
					return;
				}
				const auto parsed = Json::Parse(payload);
				if (!parsed) continue;
				const json& message = *parsed;
				// Malformed page data must not throw on the worker thread.
				const auto type = Json::Get(message, "type", "");
				if (type == msg::Heartbeat::kType) {
					heartbeatDeadline = ::GetTickCount64() +
						osfui::browser::kHeartbeatTimeoutMs;
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
				} else if (type == msg::BackUnhandled::kType) {
					const auto back = msg::FromJson<msg::BackUnhandled>(message);
					Push(Notify{ .kind = Notify::Kind::Back, .view = back.view, .presentationEpoch = back.presentationEpoch });
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
					// Ignore loads and frames after the host has failed.
					SignalDead("browser host reported a fatal error");
					return;
				} else if (type == msg::Console::kType) {
					const auto console = msg::FromJson<msg::Console>(message);
					Push(Notify{ .kind = Notify::Kind::Console,
						.view = console.view, .text = console.json });
				} else if (type == msg::Cursor::kType) {
					// Cursor callbacks run on the transport thread by contract.
					if (onCursorChange) {
						onCursorChange(msg::FromJson<msg::Cursor>(message).id);
					}
				} else if (type == msg::Log::kType) {
					const auto entry = msg::FromJson<msg::Log>(message);
					Push(Notify{ .kind = Notify::Kind::Log,
						.text = entry.text, .code = entry.level });
				} else if (type == msg::Bye::kType) {
					REX::INFO("BrowserHostClient: browser-host bye ({})",
						msg::FromJson<msg::Bye>(message).reason);
				}
			}
		}

		void OnTexturesMessage(const msg::Textures& a_msg)
		{
			// Accept announced ring depths only within the plugin's fixed capacity.
			static_assert(osfui::browser::kRingSlots <= SharedRingDesc::kMaxSlots);
			SharedRingDesc desc{};
			const auto& slots = a_msg.slots;
			const auto closeHandleValue = [](std::uint64_t a_value) {
				if (a_value != 0) {
					::CloseHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(a_value)));
				}
			};
			bool malformed = slots.empty() || slots.size() > SharedRingDesc::kMaxSlots ||
				a_msg.produceFence == 0;
			for (std::size_t i = 0; !malformed && i < slots.size(); ++i) {
				malformed = slots[i] == 0;
			}
			if (malformed) {
				for (const auto handle : slots) closeHandleValue(handle);
				closeHandleValue(a_msg.produceFence);
				SignalDead("browser host announced an invalid shared texture ring");
				return;
			}
			for (std::size_t i = 0; i < slots.size(); ++i) {
				desc.slotHandles[i] = reinterpret_cast<void*>(
					static_cast<std::uintptr_t>(slots[i]));
				++desc.slotCount;
			}
			desc.produceFence = reinterpret_cast<void*>(
				static_cast<std::uintptr_t>(a_msg.produceFence));
			desc.width = a_msg.width;
			desc.height = a_msg.height;
			desc.adapterLuidLow = a_msg.adapterLuidLow;
			desc.adapterLuidHigh = a_msg.adapterLuidHigh;
			frames->AnnounceRing(desc);
		}

		void OnFrameMessage(const msg::Frame& a_msg)
		{
			if (frames->Publish(a_msg.slot, a_msg.serial, a_msg.width, a_msg.height, a_msg.presentationEpoch, a_msg.complete) == SharedFrameState::PublishResult::Invalid) {
				SignalDead("browser host published an invalid or still-owned ring slot");
			}
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
				case Notify::Kind::Back:
					if (onBack) onBack(value.view, value.presentationEpoch);
					break;
				case Notify::Kind::Web:
					{
						bool knownView = false;
						{
							std::scoped_lock lock(stateMutex);
							knownView = FindView(value.view) != nullptr;
						}
						if (onWebMessage && knownView) onWebMessage(value.view, value.text);
					}
					break;
				case Notify::Kind::Load:
					// Ignore load and recovery notifications from a failed connection.
					if (lifecycle.load(std::memory_order_acquire) == Lifecycle::Failed) break;
					if (onLoad) {
						const LoadEvent event{
							.viewId = value.view,
							.failed = value.failed,
							.url = value.text,
							.description = value.detail,
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
				case Notify::Kind::Log:
					LogBrowserHostLine(value.code, value.text);
					break;
				case Notify::Kind::Dead:
					REX::ERROR("BrowserHostClient: browser-host connection lost — the "
							   "overlay is closing before bounded browser-host recovery begins "
							   "(browser-host log: {})", Utf8Path(browserHostLog));
					if (onFailure) {
						onFailure(FailureEvent{
							.stage = "host-connection",
							.viewId = inputTargetId,
							.description = "Browser-host connection lost",
							.errorCode = 0
						});
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
			// Lenient parse: payloads are page-authored.
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


		// Close the transport before terminating the host.
		void Stop()
		{
			auto prior = lifecycle.load(std::memory_order_acquire);
			do {
				if (prior == Lifecycle::Stopped || prior == Lifecycle::Stopping) return;
			} while (!lifecycle.compare_exchange_weak(prior, Lifecycle::Stopping, std::memory_order_acq_rel));

			// Close cancels pipe I/O so the joins cannot block.
			outbound.Close();
			pipe.Close();
			if (worker.joinable()) worker.join();
			if (writer.joinable()) writer.join();
			frames->Disconnect();

			if (const HANDLE browserHostProcess = std::exchange(hostProcess, nullptr)) {
				::TerminateProcess(browserHostProcess, 9);
				// Termination is async; wait so a restart can reuse the host's files.
				::WaitForSingleObject(browserHostProcess, 1000u);
				::CloseHandle(browserHostProcess);
			}

			lifecycle.store(Lifecycle::Stopped, std::memory_order_release);
		}
		void ResetAfterFailure()
		{
			Stop();
			// Successful preparation survives browser recovery; failed preparation can be retried.
			if (viewFilesPreparation.valid() &&
				viewFilesPreparation.wait_for(std::chrono::seconds(0)) == std::future_status::ready &&
				!viewFilesPreparation.get()) {
				viewFilesPreparation = {};
			}

			const auto discardedOut = outbound.Size();
			outbound.Reset();

			{
				std::scoped_lock lock(notifyMutex);
				notifications.clear();
			}

			{
				std::scoped_lock lock(stateMutex);
				focusRequested = false;
			}

			if (discardedOut) {
				REX::WARN("BrowserHostClient: discarded {} transient message(s) from "
						  "the dead document; runtime state will be replayed to its replacement",
					discardedOut);
			}
		}
	};

	BrowserHostClient::BrowserHostClient() :
		m_impl(std::make_unique<Impl>())
	{}

	BrowserHostClient::~BrowserHostClient()
	{
		if (m_impl) {
			m_impl->Stop();
			// Preparation may be the only active job if no view was ever opened.
			if (m_impl->viewFilesPreparation.valid()) m_impl->viewFilesPreparation.wait();
		}
	}

	bool BrowserHostClient::Initialize(const BrowserHostConfig& a_config)
	{
        m_impl->config = a_config;
        const auto layout = HostLayout(a_config.backend, a_config.dataDir);
        REX::INFO("Browser backend: {}; Starfield retains native focus", osfui::browser::BackendName(a_config.backend));
        m_impl->viewsRoot = a_config.dataDir / "views";
        m_impl->legacyViewsRoot = a_config.dataDir.parent_path().parent_path() / "OSFUI" / "views";
        m_impl->userData = LocalOsfuiDir() / layout.profile;
        m_impl->browserHostLog = SFSE::log::log_directory().value_or(LocalOsfuiDir()) / layout.log;
        m_impl->browserHostExeSource = layout.executable;
		m_impl->width = (std::max)(1u, a_config.width);
		m_impl->height = (std::max)(1u, a_config.height);
		m_impl->viewportWidth = m_impl->width;
		m_impl->viewportHeight = m_impl->height;
        std::string error;
        if (!ValidateHostPayload(a_config.backend, layout.executable, error)) {
            REX::ERROR("{}", error);
            return false;
        }
		return true;
	}

	void BrowserHostClient::PrepareViewFiles()
	{
		m_impl->PrepareViewFiles();
	}

	void BrowserHostClient::RestartAfterFailure()
	{
		m_impl->ResetAfterFailure();
	}

	void BrowserHostClient::CreateOrNavigateView(const ViewManifest& a_manifest)
	{
		const auto logicalHeight = (std::max)(1u, a_manifest.height);
		std::scoped_lock lock(m_impl->stateMutex);
		auto* view = m_impl->FindView(a_manifest.id);
		if (!view) {
			view = &m_impl->views.emplace_back();
			view->id = a_manifest.id;
		}
		view->entry = a_manifest.entry;
		view->legacy = a_manifest.legacy;
		view->logicalHeight = logicalHeight;
		// Use the first view for input until Runtime selects a target.
		if (m_impl->inputTargetId.empty()) {
			m_impl->inputTargetId = a_manifest.id;
		}
		// Re-registering an instantiated view navigates it for dev or crash recovery.
		m_impl->Send(ToJson(msg::Navigate{ .id = a_manifest.id, .entry = a_manifest.entry,
			.logicalHeight = logicalHeight, .legacy = a_manifest.legacy }));
	}

    bool BrowserHostClient::RefreshModFiles(std::string_view a_mod)
    {
        return m_impl && m_impl->RefreshModFiles(a_mod);
    }

	void BrowserHostClient::SetInputTargetView(std::string_view a_id)
	{
		std::scoped_lock lock(m_impl->stateMutex);
		if (!m_impl->FindView(a_id)) {
			REX::WARN("BrowserHostClient: SetInputTargetView('{}') ignored — view not instantiated",
				a_id);
			return;
		}
		if (m_impl->inputTargetId == a_id) return;
		m_impl->inputTargetId = a_id;
		m_impl->Send(ToJson(msg::SetInputTarget{ .view = std::string(a_id) }));
	}

	void BrowserHostClient::Resize(std::uint32_t a_width, std::uint32_t a_height)
	{
		if (!a_width || !a_height) return;
		std::scoped_lock lock(m_impl->stateMutex);
		if (m_impl->width == a_width && m_impl->height == a_height) return;
		m_impl->width = a_width;
		m_impl->height = a_height;
		m_impl->Send(ToJson(msg::Resize{ .width = a_width, .height = a_height }));
	}

	void BrowserHostClient::SetViewport(
		const std::uint32_t a_width, const std::uint32_t a_height)
	{
		if (!a_width || !a_height) return;
		std::scoped_lock lock(m_impl->stateMutex);
		if (m_impl->viewportWidth == a_width && m_impl->viewportHeight == a_height) return;
		m_impl->viewportWidth = a_width;
		m_impl->viewportHeight = a_height;
		m_impl->BeginPresentation();
		m_impl->SendViewport();
	}

	void BrowserHostClient::SetPointerInputEnabled(const bool a_enabled)
	{
		std::scoped_lock lock(m_impl->stateMutex);
		if (m_impl->pointerInputEnabled == a_enabled) return;
		m_impl->pointerInputEnabled = a_enabled;
		m_impl->Send(ToJson(msg::PointerInput{ .enabled = a_enabled }));
	}

	void BrowserHostClient::ResetPresentation()
	{
		std::scoped_lock lock(m_impl->stateMutex);
		m_impl->BeginPresentation();
		m_impl->SendViewport();
	}

	void BrowserHostClient::DrainNotifications()
	{
		m_impl->DrainNotifications();
	}

	void BrowserHostClient::Update()
	{
		if (m_impl->lifecycle.load(std::memory_order_acquire) ==
			Impl::Lifecycle::Stopped) {
			bool wantsView = false;
			{
				std::scoped_lock lock(m_impl->stateMutex);
				wantsView = !m_impl->views.empty();
			}
			if (wantsView) {
				const auto language = m_impl->config.resolveLanguage ?
					m_impl->config.resolveLanguage() : std::optional<std::string>{ std::string{} };
				if (language) {
					m_impl->language = *language;
					m_impl->Start();
				}
			}
		}
		// Tick thread, even while hidden. Release unused frames now; wait for GPU reads on recorded frames.
		const auto released = m_impl->frames->TakeReleases();
		for (std::uint32_t slot = 0; slot < released.size(); ++slot) {
			if (released[slot]) m_impl->Send(ToJson(msg::FrameAck{ .slot = slot, .serial = released[slot] }));
		}

	}

	std::shared_ptr<SharedFrameConsumer> BrowserHostClient::Frames() const
	{
		return m_impl->frames;
	}

	void BrowserHostClient::SendMessageToWeb(
		std::string_view a_viewId, std::string_view a_json)
	{
		std::scoped_lock lock(m_impl->stateMutex);
		if (!m_impl->FindView(a_viewId)) return;
		m_impl->SendOrQueue(ToJson(msg::PostWeb{ .view = std::string(a_viewId),
			.json = std::string(a_json) }));
	}

	void BrowserHostClient::SetWebMessageHandler(WebMessageHandler a_handler)
	{
		m_impl->onWebMessage = std::move(a_handler);
	}
	void BrowserHostClient::SetLoadHandler(LoadHandler a_handler)
	{
		m_impl->onLoad = std::move(a_handler);
	}
	void BrowserHostClient::SetFailureHandler(FailureHandler a_handler)
	{
		m_impl->onFailure = std::move(a_handler);
	}
	void BrowserHostClient::SetCursorChangeHandler(CursorChangeHandler a_handler)
	{
		m_impl->onCursorChange = std::move(a_handler);
	}
	void BrowserHostClient::SetInputFocus(bool a_focused)
	{
		std::scoped_lock lock(m_impl->stateMutex);
		m_impl->focusRequested = a_focused;
		m_impl->Send(ToJson(msg::Focus{ .focused = a_focused }));
	}

	void BrowserHostClient::SetBackHandler(BackHandler a_handler)
	{
		m_impl->onBack = std::move(a_handler);
	}

	void BrowserHostClient::RequestBack(std::string_view a_viewId)
	{
		std::scoped_lock lock(m_impl->stateMutex);
		m_impl->Send(ToJson(msg::Back{ .view = std::string(a_viewId), .presentationEpoch = m_impl->presentationEpoch }));
	}

	bool BrowserHostClient::IsCurrentInputPresentation(std::string_view a_viewId, std::uint64_t a_epoch) const
	{
		std::scoped_lock lock(m_impl->stateMutex);
		return m_impl->presentationEpoch == a_epoch && m_impl->inputTargetId == a_viewId &&
			!m_impl->allHidden && m_impl->focusRequested && m_impl->windowActive;
	}

	void BrowserHostClient::InjectKeyEvent(std::uint32_t a_vkCode, bool a_down)
	{
		m_impl->Send(ToJson(BrowserNavigationKeyEvent(a_vkCode, a_down)));
	}

	void BrowserHostClient::InjectKeyboard(const msg::Keyboard& a_key)
	{
		m_impl->Send(ToJson(a_key));
	}
	void BrowserHostClient::InjectText(const msg::TextInput& a_text)
	{
		m_impl->Send(ToJson(a_text));
	}
	void BrowserHostClient::SetWindowActive(bool a_active)
	{
		std::scoped_lock lock(m_impl->stateMutex);
		if (m_impl->windowActive == a_active) return;
		m_impl->windowActive = a_active;
		m_impl->Send(ToJson(msg::WindowActive{ .active = a_active }));
		REX::INFO("BrowserHostClient: game window active -> {}", a_active);
	}
	void BrowserHostClient::GameWindowChanged()
	{
		// The connection snapshot reads this handle under stateMutex too.
		std::scoped_lock lock(m_impl->stateMutex);
		m_impl->Send(ToJson(msg::GameWindow{
			.hwnd = reinterpret_cast<std::uint64_t>(OverlayInputHook::GameWindow()) }));
	}

	namespace
	{
		// Shift/Ctrl held while a mouse event is sampled, as Win32 MK_* flags.
		std::uint32_t HeldMouseModifiers()
		{
			return (::GetAsyncKeyState(VK_SHIFT) & 0x8000 ? MK_SHIFT : 0u) |
				(::GetAsyncKeyState(VK_CONTROL) & 0x8000 ? MK_CONTROL : 0u);
		}
	}

	void BrowserHostClient::InjectMouseMove(int a_x, int a_y)
	{
		m_impl->Send(ToJson(msg::Mouse{ .kind = "move", .x = a_x, .y = a_y, .modifiers = HeldMouseModifiers() }));
	}
	void BrowserHostClient::InjectMouseButton(
		int a_x, int a_y, int a_button, bool a_down)
	{
		m_impl->Send(ToJson(msg::Mouse{ .kind = "button", .x = a_x, .y = a_y,
			.button = a_button, .down = a_down, .modifiers = HeldMouseModifiers() }));
	}
	void BrowserHostClient::InjectMouseWheel(int a_x, int a_y, int a_wheelDelta)
	{
		m_impl->Send(ToJson(msg::Mouse{ .kind = "wheel", .x = a_x, .y = a_y,
			.wheel = a_wheelDelta }));
	}

	void BrowserHostClient::InjectPhysicalMouseWheel(
		int a_x, int a_y, int a_wheelDelta)
	{
		m_impl->Send(ToJson(msg::Mouse{ .kind = "wheel", .x = a_x, .y = a_y,
			.wheel = a_wheelDelta, .modifiers = HeldMouseModifiers() }));
	}

	void BrowserHostClient::OpenDevTools(std::string_view a_viewId)
	{
		m_impl->Send(ToJson(msg::OpenDevTools{ .view = std::string(a_viewId) }));
	}

	void BrowserHostClient::SetConsoleHandler(
		std::string_view a_viewId, ConsoleHandler a_handler)
	{
		if (a_handler) {
			m_impl->consoleHandlers[std::string(a_viewId)] = std::move(a_handler);
		} else {
			m_impl->consoleHandlers.erase(std::string(a_viewId));
		}
	}

	void BrowserHostClient::SetViewHidden(std::string_view a_viewId, bool a_hidden, bool a_settleColdOpen)
	{
		std::scoped_lock lock(m_impl->stateMutex);
		auto* view = m_impl->FindView(a_viewId);
		if (!view) return;
		const bool beginOpening = !a_hidden && a_settleColdOpen && !view->settleColdOpen;
		view->settleColdOpen = !a_hidden && a_settleColdOpen;
		if (view->hidden == a_hidden && !beginOpening) return;
		view->hidden = a_hidden;
		m_impl->RecomputeAllHidden();
		if (!a_hidden) {
			// Every show is a new presentation, even when another view kept the overlay visible.
			m_impl->BeginPresentation();
		} else if (m_impl->allHidden) {
			m_impl->frames->SetPresentation(m_impl->presentationEpoch, false);
		}
		m_impl->Send(ToJson(msg::SetHidden{ .view = std::string(a_viewId),
			.hidden = a_hidden, .presentationEpoch = m_impl->presentationEpoch, .settleColdOpen = view->settleColdOpen }));
	}

	void BrowserHostClient::SetViewOrder(std::string_view a_viewId, int a_order)
	{
		std::scoped_lock lock(m_impl->stateMutex);
		auto* view = m_impl->FindView(a_viewId);
		if (!view || view->order == a_order) return;
		view->order = a_order;
		m_impl->Send(ToJson(msg::SetOrder{ .view = std::string(a_viewId), .order = a_order }));
	}

	void BrowserHostClient::DestroyView(std::string_view a_viewId)
	{
		{
			std::scoped_lock lock(m_impl->stateMutex);
			const auto* view = m_impl->FindView(a_viewId);
			if (!view) return;
			std::erase_if(m_impl->views,
				[&](const Impl::ViewRec& a_rec) { return a_rec.id == a_viewId; });
			if (m_impl->inputTargetId == a_viewId) {
				m_impl->inputTargetId = m_impl->views.empty() ? std::string{} : m_impl->views.front().id;
			}
			m_impl->RecomputeAllHidden();
			if (m_impl->allHidden) {
				m_impl->frames->SetPresentation(m_impl->presentationEpoch, false);
			}
			m_impl->Send(ToJson(msg::DestroyView{ .view = std::string(a_viewId) }));
		}
		// Game-thread map; no lock.
		m_impl->consoleHandlers.erase(std::string(a_viewId));
	}
}
