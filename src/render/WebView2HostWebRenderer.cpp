#include "render/WebView2HostWebRenderer.h"

#if defined(OSFUI_WITH_WEBVIEW2)

#include <atomic>
#include <deque>
#include <random>
#include <thread>
#include <unordered_map>

#include "core/Log.h"
#include "core/Version.h"
#include "input/OverlayInputHook.h"

#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>
#include <ShlObj.h>
#include <WebView2.h>
#include <nlohmann/json.hpp>

#include "Wv2BrokerLaunch.h"
#include "Wv2Pipe.h"
#include "Wv2Protocol.h"

using nlohmann::json;

namespace OSFUI
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

		CursorShape CursorFromId(std::uint32_t a_id)
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
	}

	struct WebView2HostWebRenderer::Impl
	{
		struct Notify
		{
			enum class Kind { Web, Dom, Load, Eval, Console, Ring, Log, Dead };
			Kind           kind{ Kind::Web };
			std::string    text, detail;
			bool           failed{};
			int            code{};
			std::uint64_t  id{};
			SharedRingDesc ring{};
		};

		RendererConfig        config;
		std::filesystem::path viewsRoot, mappedViewsRoot, userData;
		std::filesystem::path hostExeSource, hostExeMirror;
		std::string           viewId;
		bool                  bridgeAllowed{};

		WebMessageHandler       onWebMessage;
		DomReadyHandler         onDomReady;
		LoadHandler             onLoad;
		CursorChangeHandler     onCursorChange;
		NativeAcceleratorHandler onAccelerator;
		ConsoleHandler          onConsole;
		SharedRingHandler       onSharedRing;
		std::unordered_map<std::string, JsListenerHandler> listeners;

		// State the worker snapshots at connect time; later changes are sent
		// as diffs from the calling thread (WriteMessage is thread-safe).
		std::mutex                  stateMutex;
		std::optional<ViewManifest> pendingManifest;
		std::uint32_t               width{ 1 }, height{ 1 };
		bool                        hidden{ true };
		// accelState mirror (SetAcceleratorKeys diffs against this)
		std::uint32_t accToggle{ 0 }, accDevReload{ 0 }, accCaptureUp{ 0 };
		bool          accCaptured{ false }, accArmed{ false }, accSent{ false };

		osfui::wv2::Pipe pipe;
		std::thread      worker;
		std::atomic_bool started{ false }, stopRequested{ false };
		std::atomic_bool connected{ false }, dead{ false };
		bool             deadLogged{ false };
		DWORD            hostPid{ 0 };
		HANDLE           hostProcess{ nullptr };
		HWND             topLevel{ nullptr };

		std::mutex         notifyMutex;
		std::deque<Notify> notifications;

		// Latest shared-ring frame (reader thread writes, game thread reads).
		std::mutex    frameMutex;
		bool          haveFrame{ false };
		std::uint32_t frameSlot{ 0 };
		std::uint64_t frameSerial{ 0 };
		std::uint32_t frameWidth{ 0 }, frameHeight{ 0 };
		std::uint64_t frameGeneration{ 0 };
		std::uint64_t submittedSerial{ 0 };
		std::uint32_t ringWidth{ 0 }, ringHeight{ 0 };
		std::uint64_t ringGeneration{ 0 };       // reader-side counter
		std::uint64_t announcedGeneration{ 0 };  // dispatched to the compositor

		std::mutex nextEvalMutex;
		std::uint64_t nextEvalId{ 0 };
		std::unordered_map<std::uint64_t, ScriptResultHandler> evalCallbacks;

		void Push(Notify a_value)
		{
			std::scoped_lock lock(notifyMutex);
			notifications.push_back(std::move(a_value));
		}

		void Send(const json& a_msg)
		{
			if (connected.load()) {
				pipe.WriteMessage(a_msg.dump());
			}
		}

		// ================= startup (worker thread) =================

		// Mod Organizer 2 presents the mod folder only inside USVFS-hooked
		// processes; the host and its browser children are deliberately
		// UNHOOKED, so both the views tree and the host exe itself must live
		// at REAL paths before anything outside the game can use them.
		void ResolveMappedViewsRoot()
		{
			mappedViewsRoot = viewsRoot;
			if (!::GetModuleHandleW(L"usvfs_x64.dll")) return;
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
				REX::WARN("WebView2HostWebRenderer: views mirror copy failed ({}); "
						  "the browser may not resolve the direct path", ec.message());
				return;
			}
			mappedViewsRoot = mirror;
			REX::INFO("WebView2HostWebRenderer: USVFS detected — views mirrored to {}",
				ToUtf8(mirror.native()));
		}

		// The host exe ships inside the mod folder (VFS-only under MO2) but is
		// launched by Explorer/the task scheduler, which cannot see the VFS.
		// Mirror it to a real, VERSIONED path first.
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
			return true;
		}

		bool Start()
		{
			if (started.exchange(true)) return true;
			REX::INFO("WebView2HostWebRenderer: starting host connection worker");
			worker = std::thread([this] { WorkerMain(); });
			return true;
		}

		void WorkerMain()
		{
			ResolveMappedViewsRoot();
			if (!MirrorHostExe()) {
				dead.store(true);
				Push(Notify{ .kind = Notify::Kind::Dead });
				return;
			}
			topLevel = FindTopLevelWindow();

			std::mt19937_64 rng(::GetTickCount64() ^
				(static_cast<std::uint64_t>(::GetCurrentProcessId()) << 17));
			const auto nonce = static_cast<std::uint32_t>(rng());
			const auto pipeName = std::format(L"{}{}-{:08x}",
				osfui::wv2::kPipePrefix, ::GetCurrentProcessId(), nonce);
			const auto hostLog = LocalOsfuiDir() / "webview2-host.log";
			const auto args = std::format(L"--pipe={} --game-pid={} --log=\"{}\"",
				pipeName, ::GetCurrentProcessId(), hostLog.native());

			// Direct spawn is only safe without USVFS: MO2 injects into every
			// child of this process and the injection crashes the WebView2
			// broker. With USVFS present, ONLY out-of-tree brokers are viable.
			const bool usvfs = ::GetModuleHandleW(L"usvfs_x64.dll") != nullptr;
			const auto launch = osfui::wv2::LaunchDetached(
				hostExeMirror.native(), args, /*a_preferBroker=*/usvfs);
			if (!launch.ok) {
				REX::ERROR("WebView2HostWebRenderer: host launch failed [{}]", launch.detail);
				dead.store(true);
				Push(Notify{ .kind = Notify::Kind::Dead });
				return;
			}
			REX::INFO("WebView2HostWebRenderer: host launched via {} (usvfs={}){}",
				osfui::wv2::LaunchMethodName(launch.method), usvfs,
				launch.detail.empty() ? "" : " detail=[" + launch.detail + "]");

			if (!pipe.CreateServerAndWait(pipeName, 20000)) {
				REX::ERROR("WebView2HostWebRenderer: host never connected: {} "
						   "(host log: {})", pipe.LastErrorText(), hostLog.string());
				dead.store(true);
				Push(Notify{ .kind = Notify::Kind::Dead });
				return;
			}

			std::string payload;
			if (!pipe.ReadMessage(payload)) {
				REX::ERROR("WebView2HostWebRenderer: no hello from host");
				dead.store(true);
				Push(Notify{ .kind = Notify::Kind::Dead });
				return;
			}
			const json hello = json::parse(payload, nullptr, false);
			if (hello.is_discarded() || hello.value("type", "") != "hello" ||
				hello.value("protocolVersion", 0u) != osfui::wv2::kProtocolVersion) {
				REX::ERROR("WebView2HostWebRenderer: bad hello (protocol mismatch?)");
				dead.store(true);
				Push(Notify{ .kind = Notify::Kind::Dead });
				return;
			}
			hostPid = hello.value("pid", 0u);
			if (hostPid) {
				hostProcess = ::OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, hostPid);
			}
			REX::INFO("WebView2HostWebRenderer: host pid {} up (WebView2 runtime {})",
				hostPid, hello.value("runtimeVersion", "?"));
			connected.store(true);

			// Connect-time snapshot of everything the game set before the host
			// existed. Diffs sent later by the game thread may interleave with
			// this — both carry current values, so last-write-wins is fine.
			{
				std::scoped_lock lock(stateMutex);
				pipe.WriteMessage(json{
					{ "type", "init" },
					{ "topLevelHwnd", reinterpret_cast<std::uint64_t>(topLevel) },
					{ "viewsPath", ToUtf8(mappedViewsRoot.native()) },
					{ "virtualHost", "osfui.local" },
					{ "width", width },
					{ "height", height },
					{ "userDataDir", ToUtf8(userData.native()) },
					{ "devMode", config.devMode },
					{ "hidden", hidden },
				}.dump());
				pipe.WriteMessage(json{
					{ "type", "accelState" },
					{ "toggleVk", accToggle }, { "devReloadVk", accDevReload },
					{ "captured", accCaptured }, { "captureArmed", accArmed },
					{ "captureUpVk", accCaptureUp } }.dump());
				accSent = true;
				if (pendingManifest) {
					pipe.WriteMessage(json{
						{ "type", "navigate" },
						{ "id", pendingManifest->id },
						{ "entry", pendingManifest->entry },
						{ "bridge", pendingManifest->permissions.nativeBridge } }.dump());
				}
			}

			ReadLoop();

			connected.store(false);
			if (!stopRequested.load()) {
				dead.store(true);
				Push(Notify{ .kind = Notify::Kind::Dead });
			}
		}

		// ================= inbound (worker thread) =================

		void ReadLoop()
		{
			std::string payload;
			while (!stopRequested.load() && pipe.ReadMessage(payload)) {
				const json msg = json::parse(payload, nullptr, false);
				if (msg.is_discarded()) continue;
				const std::string type = msg.value("type", "");
				if (type == "frame") {
					OnFrameMessage(msg);
				} else if (type == "textures") {
					OnTexturesMessage(msg);
				} else if (type == "webMessage") {
					Push(Notify{ .kind = Notify::Kind::Web,
						.text = msg.value("json", "") });
				} else if (type == "domReady") {
					Push(Notify{ .kind = Notify::Kind::Dom });
				} else if (type == "loadEvent") {
					Push(Notify{ .kind = Notify::Kind::Load,
						.text = msg.value("url", ""),
						.detail = msg.value("description", ""),
						.failed = msg.value("failed", false),
						.code = msg.value("code", 0) });
				} else if (type == "console") {
					Push(Notify{ .kind = Notify::Kind::Console,
						.text = msg.value("json", "") });
				} else if (type == "cursor") {
					// Contract allows renderer-thread delivery for cursor.
					if (onCursorChange) {
						onCursorChange(CursorFromId(msg.value("id", 0u)));
					}
				} else if (type == "accelerator") {
					// Same threading as the in-process backend: the handler is
					// invoked off the game thread and must stay cheap.
					if (onAccelerator) {
						onAccelerator(msg.value("vk", 0u), msg.value("down", false));
					}
				} else if (type == "evalResult") {
					Push(Notify{ .kind = Notify::Kind::Eval,
						.text = msg.value("result", ""),
						.id = msg.value("id", 0ull) });
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
			}
		}

		void OnTexturesMessage(const json& a_msg)
		{
			SharedRingDesc desc{};
			const auto& slots = a_msg.at("slots");
			for (std::size_t i = 0; i < SharedRingDesc::kSlots && i < slots.size(); ++i) {
				desc.slotHandles[i] = reinterpret_cast<void*>(
					static_cast<std::uintptr_t>(slots[i].get<std::uint64_t>()));
			}
			desc.produceFence = reinterpret_cast<void*>(
				static_cast<std::uintptr_t>(a_msg.value("produceFence", 0ull)));
			desc.consumeFence = reinterpret_cast<void*>(
				static_cast<std::uintptr_t>(a_msg.value("consumeFence", 0ull)));
			desc.width = a_msg.value("width", 0u);
			desc.height = a_msg.value("height", 0u);
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
			const auto w = a_msg.value("width", 0u);
			const auto h = a_msg.value("height", 0u);
			std::uint64_t ackSerial = 0;
			bool ackNew = false;
			{
				std::scoped_lock lock(frameMutex, stateMutex);
				if (w != ringWidth || h != ringHeight) {
					ackNew = true;  // stale ring — release the slot immediately
				} else {
					if (haveFrame && frameSerial != submittedSerial) {
						// The previous frame was never handed to the compositor;
						// nothing will ever signal its consumption — ack it.
						ackSerial = frameSerial;
					}
					frameSlot = slot;
					frameSerial = serial;
					frameWidth = w;
					frameHeight = h;
					frameGeneration = ringGeneration;
					haveFrame = true;
					if (hidden) {
						// Render() is not called while hidden; the host
						// republishes on unhide, so this serial is disposable.
						ackNew = true;
					}
				}
			}
			if (ackSerial) {
				pipe.WriteMessage(json{
					{ "type", "frameAck" }, { "serial", ackSerial } }.dump());
			}
			if (ackNew) {
				pipe.WriteMessage(json{
					{ "type", "frameAck" }, { "serial", serial } }.dump());
			}
		}

		// ================= game-thread dispatch =================

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
						const auto parsed = json::parse(value.text);
						if (parsed.contains("__osfuiListener")) {
							const auto name = parsed.value("__osfuiListener", "");
							const auto found = listeners.find(name);
							if (found != listeners.end() && found->second)
								found->second(parsed.value("argument", ""));
							break;
						}
					} catch (...) {}
					if (onWebMessage && bridgeAllowed) onWebMessage(viewId, value.text);
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
							.errorDomain = "WebView2Host",
							.errorCode = value.code
						};
						onLoad(event);
					}
					break;
				case Notify::Kind::Eval: {
					ScriptResultHandler callback;
					{
						std::scoped_lock lock(nextEvalMutex);
						if (const auto it = evalCallbacks.find(value.id);
							it != evalCallbacks.end()) {
							callback = std::move(it->second);
							evalCallbacks.erase(it);
						}
					}
					if (callback) callback(std::move(value.text));
					break;
				}
				case Notify::Kind::Console:
					DeliverConsole(value.text);
					break;
				case Notify::Kind::Ring:
					announcedGeneration = value.ring.generation;
					if (onSharedRing) {
						onSharedRing(value.ring);
					} else {
						// Nobody adopts the handles — close them so they don't leak.
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
								   "overlay stays hidden for the rest of this session "
								   "(host log: {})",
							(LocalOsfuiDir() / "webview2-host.log").string());
					}
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
			onConsole(level, std::move(text));
		}

		// ================= teardown =================

		void Stop()
		{
			if (!started.load()) return;
			stopRequested.store(true);
			if (connected.load()) {
				pipe.WriteMessage(json{ { "type", "shutdown" } }.dump());
			}
			// Bounded wait for a clean host exit. This runs on the SFSE main
			// thread, NOT the game's window thread, so the host's teardown of
			// its game-parented HWND cannot deadlock against us.
			if (hostProcess) {
				if (::WaitForSingleObject(hostProcess, 3000) != WAIT_OBJECT_0) {
					REX::WARN("WebView2HostWebRenderer: host did not exit in 3s — terminating");
					::TerminateProcess(hostProcess, 9);
					::WaitForSingleObject(hostProcess, 1000);
				}
				::CloseHandle(hostProcess);
				hostProcess = nullptr;
			}
			pipe.Close();
			if (worker.joinable()) worker.join();
			started.store(false);
		}
	};

	WebView2HostWebRenderer::WebView2HostWebRenderer() :
		_impl(std::make_unique<Impl>())
	{}

	WebView2HostWebRenderer::~WebView2HostWebRenderer() { Shutdown(); }

	bool WebView2HostWebRenderer::RuntimeAvailable()
	{
		wchar_t forced[2]{};
		if (::GetEnvironmentVariableW(
				L"OSFUI_WEBVIEW2_FORCE_RUNTIME_ABSENT", forced, 2) > 0)
			return false;
		LPWSTR version = nullptr;
		const auto result =
			::GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
		if (FAILED(result) || !version) return false;
		REX::INFO("WebView2HostWebRenderer: evergreen runtime {}", ToUtf8(version));
		::CoTaskMemFree(version);
		return true;
	}

	bool WebView2HostWebRenderer::Initialize(const RendererConfig& a_config)
	{
		if (!RuntimeAvailable()) return false;
		_impl->config = a_config;
		_impl->viewsRoot = a_config.dataDir / "views";
		_impl->userData = LocalOsfuiDir() / "WebView2";
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

	void WebView2HostWebRenderer::Shutdown()
	{
		if (_impl) _impl->Stop();
	}

	void WebView2HostWebRenderer::LoadView(const ViewManifest& a_manifest)
	{
		{
			std::scoped_lock lock(_impl->stateMutex);
			if (!_impl->viewId.empty() && _impl->viewId != a_manifest.id) {
				REX::WARN("WebView2HostWebRenderer: single view; ignoring '{}'",
					a_manifest.id);
				return;
			}
			_impl->viewId = a_manifest.id;
			_impl->bridgeAllowed = a_manifest.permissions.nativeBridge;
			_impl->pendingManifest = a_manifest;
		}
		_impl->Send(json{
			{ "type", "navigate" },
			{ "id", a_manifest.id },
			{ "entry", a_manifest.entry },
			{ "bridge", a_manifest.permissions.nativeBridge } });
	}

	void WebView2HostWebRenderer::SetActiveView(std::string_view a_id)
	{
		if (!_impl->viewId.empty() && a_id != _impl->viewId)
			REX::DEBUG("WebView2HostWebRenderer: ignored active view '{}' (single-view)",
				a_id);
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

	void WebView2HostWebRenderer::Update(double)
	{
		// Warm start once a view is configured (mirrors the in-process
		// backend): the mirror copies, broker launch, environment creation,
		// and navigation all happen while the overlay is still hidden.
		if (!_impl->started.load() && !_impl->dead.load()) {
			bool wantsView = false;
			{
				std::scoped_lock lock(_impl->stateMutex);
				wantsView = _impl->pendingManifest.has_value();
			}
			if (wantsView) _impl->Start();
		}
		_impl->DrainNotifications();
	}

	std::optional<FrameBufferView> WebView2HostWebRenderer::Render()
	{
		std::scoped_lock lock(_impl->frameMutex);
		if (!_impl->haveFrame ||
			_impl->frameGeneration != _impl->announcedGeneration) {
			// No frame, or its ring has not been announced to the compositor
			// yet (the Ring notification dispatches from Update()).
			return std::nullopt;
		}
		_impl->submittedSerial = _impl->frameSerial;
		return FrameBufferView{
			.pixels = {},
			.width = _impl->frameWidth,
			.height = _impl->frameHeight,
			.strideBytes = 0,
			.format = PixelFormat::kBGRA8,
			.frameIndex = _impl->frameSerial,
			.dirty = DirtyRect::Full(_impl->frameWidth, _impl->frameHeight),
			.sharedSlot = static_cast<std::int32_t>(_impl->frameSlot),
		};
	}

	void WebView2HostWebRenderer::SendMessageToWeb(
		std::string_view a_viewId, std::string_view a_json)
	{
		if (a_viewId != _impl->viewId || !_impl->bridgeAllowed) return;
		_impl->Send(json{ { "type", "postWeb" }, { "json", std::string(a_json) } });
	}

	void WebView2HostWebRenderer::SetWebMessageHandler(WebMessageHandler a_handler)
	{
		_impl->onWebMessage = std::move(a_handler);
	}
	void WebView2HostWebRenderer::SetDomReadyHandler(DomReadyHandler a_handler)
	{
		_impl->onDomReady = std::move(a_handler);
	}
	void WebView2HostWebRenderer::SetLoadHandler(LoadHandler a_handler)
	{
		_impl->onLoad = std::move(a_handler);
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

	void WebView2HostWebRenderer::SetNativeKeyboardFocus(bool a_focused)
	{
		if (a_focused) {
			_impl->Start();
		}
		_impl->Send(json{ { "type", "focus" }, { "focused", a_focused } });
		if (!a_focused && _impl->topLevel) {
			// Restore game focus on the game's own window thread, exactly like
			// the in-process backend.
			::PostMessageW(_impl->topLevel,
				OverlayInputHook::kRestoreGameFocusMessage, 0, 0);
		}
	}

	void WebView2HostWebRenderer::SetAcceleratorKeys(std::uint32_t a_toggleVk,
		std::uint32_t a_devReloadVk, bool a_captured, bool a_captureArmed,
		std::uint32_t a_captureUpVk)
	{
		bool changed = false;
		{
			std::scoped_lock lock(_impl->stateMutex);
			changed = !_impl->accSent || _impl->accToggle != a_toggleVk ||
				_impl->accDevReload != a_devReloadVk ||
				_impl->accCaptured != a_captured ||
				_impl->accArmed != a_captureArmed ||
				_impl->accCaptureUp != a_captureUpVk;
			_impl->accToggle = a_toggleVk;
			_impl->accDevReload = a_devReloadVk;
			_impl->accCaptured = a_captured;
			_impl->accArmed = a_captureArmed;
			_impl->accCaptureUp = a_captureUpVk;
			if (changed && _impl->connected.load()) _impl->accSent = true;
		}
		if (changed) {
			_impl->Send(json{
				{ "type", "accelState" },
				{ "toggleVk", a_toggleVk }, { "devReloadVk", a_devReloadVk },
				{ "captured", a_captured }, { "captureArmed", a_captureArmed },
				{ "captureUpVk", a_captureUpVk } });
		}
	}

	void WebView2HostWebRenderer::InjectKeyEvent(std::uint32_t a_vkCode, bool a_down)
	{
		// Synthetic keys (gamepad nav taps, Esc back-delegation). Real typing
		// never comes through here — it rides real OS focus.
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

	void WebView2HostWebRenderer::EvaluateScript(
		std::string_view a_viewId, std::string_view a_js,
		ScriptResultHandler a_onResult)
	{
		if (a_viewId != _impl->viewId) return;
		std::uint64_t id = 0;
		{
			std::scoped_lock lock(_impl->nextEvalMutex);
			id = ++_impl->nextEvalId;
			if (a_onResult) {
				_impl->evalCallbacks[id] = std::move(a_onResult);
			}
		}
		_impl->Send(json{
			{ "type", "eval" }, { "id", id }, { "script", std::string(a_js) } });
	}

	void WebView2HostWebRenderer::CallJsFunction(
		std::string_view a_viewId, std::string_view a_fnName, std::string_view a_arg)
	{
		const auto name = json(std::string(a_fnName)).dump();
		const auto arg = json(std::string(a_arg)).dump();
		EvaluateScript(a_viewId,
			"typeof window[" + name + "]==='function' ? window[" +
			name + "](" + arg + ") : undefined");
	}

	void WebView2HostWebRenderer::RegisterJsFunction(
		std::string_view a_viewId, std::string_view a_name,
		JsListenerHandler a_callback)
	{
		if (a_viewId != _impl->viewId) return;
		_impl->listeners[std::string(a_name)] = std::move(a_callback);
		const auto name = json(std::string(a_name)).dump();
		EvaluateScript(a_viewId,
			"window[" + name + "]=function(a){if(window.osfui&&"
			"window.osfui.__invokeListener)window.osfui.__invokeListener(" +
			name + ",a===undefined?'':String(a));};");
	}

	void WebView2HostWebRenderer::SetConsoleHandler(
		std::string_view a_viewId, ConsoleHandler a_handler)
	{
		if (a_viewId == _impl->viewId)
			_impl->onConsole = std::move(a_handler);
	}

	void WebView2HostWebRenderer::SetViewHidden(std::string_view a_viewId, bool a_hidden)
	{
		if (a_viewId != _impl->viewId) return;
		{
			std::scoped_lock lock(_impl->stateMutex);
			_impl->hidden = a_hidden;
		}
		_impl->Send(json{ { "type", "setHidden" }, { "hidden", a_hidden } });
	}

	void WebView2HostWebRenderer::DestroyView(std::string_view a_viewId)
	{
		if (a_viewId != _impl->viewId) return;
		_impl->Send(json{ { "type", "destroyView" } });
	}
}

#endif
