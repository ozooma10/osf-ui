#include "render/UltralightWebRenderer.h"

#if defined(OSFUI_WITH_ULTRALIGHT)

	#include "core/Log.h"
	#include "platform/WindowsPlatform.h"

	#include <algorithm>
	#include <cctype>
	#include <condition_variable>
	#include <deque>
	#include <thread>
	#include <utility>

	// SDK headers are not C4100-clean under /Wall-ish warning levels.
	#pragma warning(push)
	#pragma warning(disable : 4100)
	#include <AppCore/Platform.h>
	#include <JavaScriptCore/JavaScript.h>
	#include <Ultralight/Ultralight.h>
	#pragma warning(pop)

namespace OSFUI
{
	namespace
	{
		namespace ul = ultralight;

		constexpr std::size_t kMaxQueuedMessages = 64;
		constexpr std::size_t kMaxLoggedTextLen = 512;
		constexpr auto        kWorkerFrameInterval = std::chrono::milliseconds(16);  // ~60 Hz
		constexpr int         kWheelDelta = 120;  // one notch (Win32 WHEEL_DELTA), kept local — Windows.h is not included here

		[[nodiscard]] std::string ToUTF8(const ul::String& a_str)
		{
			const auto& s8 = a_str.utf8();
			return std::string(s8.data(), s8.length());
		}

		// Encode one Unicode scalar value as UTF-8. Used to build the text of a
		// Char event from the OS WM_CHAR/WM_UNICHAR codepoint (surrogate halves
		// are already combined upstream). Returns empty for an invalid scalar
		// (lone surrogate or out of range) so the caller can drop it.
		[[nodiscard]] std::string CodepointToUTF8(std::uint32_t a_cp)
		{
			std::string out;
			if (a_cp > 0x10FFFF || (a_cp >= 0xD800 && a_cp <= 0xDFFF)) {
				return out;
			}
			if (a_cp <= 0x7F) {
				out.push_back(static_cast<char>(a_cp));
			} else if (a_cp <= 0x7FF) {
				out.push_back(static_cast<char>(0xC0 | (a_cp >> 6)));
				out.push_back(static_cast<char>(0x80 | (a_cp & 0x3F)));
			} else if (a_cp <= 0xFFFF) {
				out.push_back(static_cast<char>(0xE0 | (a_cp >> 12)));
				out.push_back(static_cast<char>(0x80 | ((a_cp >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (a_cp & 0x3F)));
			} else {
				out.push_back(static_cast<char>(0xF0 | (a_cp >> 18)));
				out.push_back(static_cast<char>(0x80 | ((a_cp >> 12) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | ((a_cp >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (a_cp & 0x3F)));
			}
			return out;
		}

		[[nodiscard]] std::string JSStringToUTF8(JSStringRef a_str)
		{
			if (!a_str) {
				return {};
			}
			const auto max = JSStringGetMaximumUTF8CStringSize(a_str);
			std::string buffer(max, '\0');
			const auto written = JSStringGetUTF8CString(a_str, buffer.data(), buffer.size());
			buffer.resize(written > 0 ? written - 1 : 0);  // drop the NUL
			return buffer;
		}

		[[nodiscard]] std::string JSValueToUTF8(JSContextRef a_ctx, JSValueRef a_value)
		{
			JSStringRef str = JSValueToStringCopy(a_ctx, a_value, nullptr);
			if (!str) {
				return {};
			}
			auto result = JSStringToUTF8(str);
			JSStringRelease(str);
			return result;
		}

		// File access policy for the web view (docs/security-model.md): only
		// two directory trees are ever readable, the active view's folder and
		// the Ultralight support-resources folder (ICU data). Absolute paths,
		// rooted paths, and any ".." component are rejected before disk I/O.
		class SandboxFileSystem final : public ul::FileSystem
		{
		public:
			void SetResourceBase(std::filesystem::path a_dir) { _resourceBase = std::move(a_dir); }
			void SetViewsBase(std::filesystem::path a_dir) { _viewsBase = std::move(a_dir); }

			bool FileExists(const ul::String& a_path) override
			{
				const auto resolved = Resolve(a_path);
				std::error_code ec;
				return resolved && std::filesystem::is_regular_file(*resolved, ec);
			}

			ul::String GetFileMimeType(const ul::String& a_path) override
			{
				const auto resolved = Resolve(a_path);
				if (!resolved) {
					return "application/unknown";
				}
				auto ext = resolved->extension().string();
				std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				static const std::unordered_map<std::string, const char*> kMimeTypes{
					{ ".html", "text/html" },
					{ ".htm", "text/html" },
					{ ".css", "text/css" },
					{ ".js", "text/javascript" },
					{ ".mjs", "text/javascript" },
					{ ".json", "application/json" },
					{ ".png", "image/png" },
					{ ".jpg", "image/jpeg" },
					{ ".jpeg", "image/jpeg" },
					{ ".gif", "image/gif" },
					{ ".webp", "image/webp" },
					{ ".svg", "image/svg+xml" },
					{ ".ico", "image/x-icon" },
					{ ".woff", "font/woff" },
					{ ".woff2", "font/woff2" },
					{ ".ttf", "font/ttf" },
					{ ".otf", "font/otf" },
					{ ".txt", "text/plain" },
					{ ".dat", "application/octet-stream" },
					{ ".pem", "application/x-pem-file" },
				};
				const auto it = kMimeTypes.find(ext);
				return it != kMimeTypes.end() ? it->second : "application/unknown";
			}

			ul::String GetFileCharset(const ul::String&) override { return "utf-8"; }

			ul::RefPtr<ul::Buffer> OpenFile(const ul::String& a_path) override
			{
				const auto resolved = Resolve(a_path);
				if (!resolved) {
					REX::WARN("UltralightFS: refused path outside sandbox: '{}'", ToUTF8(a_path).substr(0, kMaxLoggedTextLen));
					return nullptr;
				}
				std::ifstream file(*resolved, std::ios::binary | std::ios::ate);
				if (!file) {
					return nullptr;
				}
				const auto size = static_cast<std::size_t>(file.tellg());
				file.seekg(0);
				std::vector<char> data(size);
				if (size > 0 && !file.read(data.data(), static_cast<std::streamsize>(size))) {
					return nullptr;
				}
				// CreateFromCopy guarantees the 16-byte alignment the ICU data
				// file requires (see FileSystem.h notes).
				return ul::Buffer::CreateFromCopy(data.data(), data.size());
			}

		private:
			// Maps the relative URL path Ultralight asks for to a real file,
			// or nullopt if the request escapes the sandbox.
			[[nodiscard]] std::optional<std::filesystem::path> Resolve(const ul::String& a_path) const
			{
				const auto utf8 = ToUTF8(a_path);
				if (utf8.empty()) {
					return std::nullopt;
				}
				const std::filesystem::path rel{
					std::u8string_view{ reinterpret_cast<const char8_t*>(utf8.data()), utf8.size() }
				};
				if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) {
					return std::nullopt;
				}
				for (const auto& part : rel) {
					if (part == "..") {
						return std::nullopt;
					}
				}
				// "resources/..." is served from the support dir (ICU). Everything
				// else is "<viewFolder>/<asset>" served from the shared views base:
				// the folder-qualified URL lets several views load concurrently
				// without a single mutable "current root" (which raced, so one view
				// loaded another view's files). Trade-off: a view can read a sibling
				// view's local assets (docs/security-model.md) — acceptable for
				// local mod content; the outer boundaries (no abs paths, no '..',
				// no network) are unchanged.
				const bool isResource = rel.begin() != rel.end() && *rel.begin() == "resources";
				const auto& base = isResource ? _resourceBase : _viewsBase;
				if (base.empty()) {
					return std::nullopt;
				}
				return base / rel;
			}

			std::filesystem::path _resourceBase;  // contains resources/ (ICU data)
			std::filesystem::path _viewsBase;     // parent of all view folders (data/views)
		};

		// Routes Ultralight's own diagnostics into the SFSE log.
		class RexLogger final : public ul::Logger
		{
		public:
			void LogMessage(ul::LogLevel a_level, const ul::String& a_message) override
			{
				const auto text = ToUTF8(a_message).substr(0, kMaxLoggedTextLen);
				switch (a_level) {
				case ul::LogLevel::Error:
					REX::ERROR("Ultralight: {}", text);
					break;
				case ul::LogLevel::Warning:
					REX::WARN("Ultralight: {}", text);
					break;
				default:
					REX::DEBUG("Ultralight: {}", text);
					break;
				}
			}
		};

		// System clipboard for in-page copy/cut/paste. Ultralight's editor calls
		// these from the worker thread when a focused field receives Ctrl+C/X/V:
		// the RawKeyDown carries kMod_CtrlKey + unmodified_text so WebCore fires
		// the editing command, while Windows emits a control char (<0x20) for
		// Ctrl+letter that the WM_CHAR path filters out — so no stray text is
		// typed (see ProcessKeyInput). Char16 == wchar_t on Windows, so the
		// bridge to the UTF-16 Win32 clipboard is a straight reinterpret.
		class WinClipboard final : public ul::Clipboard
		{
		public:
			void Clear() override
			{
				Platform::ClearClipboard();
			}

			ul::String ReadPlainText() override
			{
				const std::wstring text = Platform::GetClipboardText();
				if (!_loggedRead.exchange(true)) {
					REX::INFO("WinClipboard: first paste read ({} chars)", text.size());
				}
				if (text.empty()) {
					return ul::String();
				}
				const ul::String16 utf16(reinterpret_cast<const unsigned short*>(text.data()), text.size());
				return ul::String(utf16);
			}

			void WritePlainText(const ul::String& a_text) override
			{
				if (!_loggedWrite.exchange(true)) {
					REX::INFO("WinClipboard: first copy/cut write ({} bytes)", a_text.utf8().length());
				}
				const ul::String16 utf16 = a_text.utf16();
				Platform::SetClipboardText(std::wstring(utf16.udata(), utf16.udata() + utf16.length()));
			}

		private:
			std::atomic_bool _loggedRead{ false };
			std::atomic_bool _loggedWrite{ false };
		};
	}

	struct UltralightWebRenderer::Impl final :
		public ul::LoadListener,
		public ul::ViewListener
	{
		// ---- renderer-global state ----
		// One Renderer/Session/worker/FileSystem serves every View (Ultralight
		// allows only one Renderer per process). Everything that differs per
		// view lives in ViewState below; this struct owns only what is shared.
		RendererConfig    config;
		WebMessageHandler onWebMessage;

		std::mutex              mutex;  // guards `views`, `activeViewId`, and every ViewState's queues/pending ops
		std::condition_variable wake;
		bool                    stopRequested{ false };
		// Set when a hide/show changes what the composite should contain without
		// repainting any view; forces one recomposite so a hidden static view
		// doesn't linger (no view goes dirty on its own in that case).
		std::atomic_bool        compositeDirty{ false };

		std::thread              worker;
		ul::RefPtr<ul::Renderer> renderer;
		ul::RefPtr<ul::Session>  session;
		// NOTE: SandboxFileSystem is a Platform-global with a single view root,
		// so it sandboxes exactly one view at a time. Hosting several views with
		// different roots needs a per-request view mapping — a multi-view TODO
		// (docs/renderer-plan.md "Multi-view feasibility").
		SandboxFileSystem*       fileSystem{ nullptr };  // owned by Platform for process lifetime

		std::mutex frameMutex;  // guards every ViewState's pendingFrame/pendingFresh

		// ---- per-view payload types ----
		struct KeyInput
		{
			// Two streams share this one FIFO so a key's RawKeyDown always
			// precedes its Char (what WebCore needs): kVirtualKey from the VK
			// stream (WM_KEYDOWN/UP) and kChar from the OS char stream (WM_CHAR/
			// WM_UNICHAR), already layout-/dead-key-/AltGr-resolved by Windows.
			enum class Kind { kVirtualKey, kChar };
			Kind          kind{ Kind::kVirtualKey };
			std::uint32_t vk{ 0 };         // kVirtualKey: Windows VK_* code
			bool          down{ false };   // kVirtualKey: press vs release
			std::uint32_t codepoint{ 0 };  // kChar: Unicode scalar value
		};

		struct MouseInput
		{
			enum class Kind { kMove, kDown, kUp, kScroll };
			Kind kind{ Kind::kMove };
			int  x{ 0 };
			int  y{ 0 };
			int  button{ 0 };      // MouseButton order: 0=left, 1=right, 2=middle
			int  wheelDelta{ 0 };  // signed WHEEL_DELTA multiple, for kScroll
		};

		struct Frame
		{
			std::vector<std::uint8_t> pixels;
			std::uint32_t             width{ 0 };
			std::uint32_t             height{ 0 };
			std::uint32_t             strideBytes{ 0 };
			std::uint64_t             index{ 0 };
		};

		// ---- per-view JS op / notify payloads ----
			struct EvalRequest
			{
				std::uint64_t evalId{ 0 };  // 0 = fire-and-forget (Invoke with no result callback)
				std::string   script;
			};
			struct ConsoleNotify
			{
				std::string viewId;
				int         level{ 0 };  // 0=log 1=warning 2=error 3=debug 4=info
				std::string message;
			};
			struct ListenerCall
			{
				std::string viewId;
				std::string name;
				std::string arg;
			};

			// ---- per-view state (one per loaded view, keyed by manifest id) ----
		// Today exactly one view is hosted (the configured view = activeViewId)
		// and the IWebRenderer interface stays single-view, targeting the active
		// view. The keyed map + per-view grouping is the structural seam for
		// hosting several at once (renderer-plan.md M2.1) without reshaping the
		// threading model.
		struct ViewState
		{
			std::string  id;
			std::atomic<std::int32_t> zorder{ 0 };  // layering; lower composites beneath. Mutable at runtime via SetViewOrder.
			std::atomic<bool> hidden{ false };   // SetViewHidden: skipped in compositing when true.
			std::atomic<int>  scrollPx{ 28 };    // per-view scroll step (SetScrollPixelSize; stored, wheel routing not wired yet).
			std::atomic<bool> wantsConsole{ false };  // a console handler is registered for this view.
			bool         interactive{ true };    // may receive input/focus. Set at load, then immutable.
			bool         bridgeAllowed{ false };  // manifest permissions.nativeBridge; gates window.osfui. Immutable after load.

			// shared with the game thread (guarded by Impl::mutex)
			std::optional<ViewManifest>                            pendingLoad;
			std::optional<std::pair<std::uint32_t, std::uint32_t>> pendingResize;
			std::deque<std::string>                                toWeb;     // game -> page
			std::deque<std::string>                                toNative;  // page -> game
			std::deque<KeyInput>                                   toInput;   // game -> view (keyboard)
			std::deque<MouseInput>                                 toMouse;   // game -> view (mouse)
			std::deque<EvalRequest>                               toEval;    // game -> view: JS eval (EvaluateScript)
			std::deque<std::pair<std::string, std::string>>      toInterop; // game -> view: window.<fn>(arg) (CallJsFunction)
			std::deque<std::string>                              toBindListener;  // game -> view: bind window.<name> (RegisterJsFunction)

			// frame exchange: pendingFrame/pendingFresh guarded by Impl::frameMutex;
			// frontFrame is game-thread-only and backs the FrameBufferView we hand out.
			Frame pendingFrame;
			bool  pendingFresh{ false };
			Frame frontFrame;

			// worker-thread-only
			ul::RefPtr<ul::View>    view;
			Frame                   backFrame;   // latest harvested pixels (overwritten on repaint)
			bool                    backDirty{ false };  // backFrame changed since last expose/composite
			std::uint64_t           paintCount{ 0 };
			bool                    domReady{ false };
			std::deque<std::string> stashedToWeb;  // arrived before the page was ready
			bool                    dumpedFirstFrame{ false };
			// keyboard modifier state (tracked from the VK stream)
			bool modShift{ false };
			bool modCtrl{ false };
			bool modAlt{ false };
			bool loggedFirstKey{ false };
		};
		std::unordered_map<std::string, std::unique_ptr<ViewState>> views;         // guarded by mutex
		std::string                                                 activeViewId;  // guarded by mutex
		std::vector<std::string>                                    drawOrder;     // view ids bottom->top; guarded by mutex

		// Composited output, used ONLY when more than one view is hosted. The
		// worker blends every view's latest frame (drawOrder, bottom-to-top) into
		// compositedBack, then hands it to the game thread exactly like a single
		// view's frame. compositedPending/Fresh are guarded by frameMutex;
		// compositedFront is game-thread-only. With one view this stays unused and
		// the single-view fast path (per-view frame exchange) is untouched.
		Frame         compositedBack;
		Frame         compositedPending;
		bool          compositedFresh{ false };
		Frame         compositedFront;
		std::uint64_t compositeIndex{ 0 };

		// The JSC postMessage callback is a plain function pointer with no
		// user-data slot, so it finds us through this. One renderer per process.
		// (Attributing a message to its SOURCE view, when several are hosted,
		// will key on the JSContextRef; today it routes to the active view.)
		static inline std::atomic<Impl*> sActive{ nullptr };

		// ---- per-view JS interaction plumbing ----
		// game-thread-set handlers + maps (touched only on the game thread):
		DomReadyHandler                                        onDomReady;
		LoadHandler                                           onLoad;  // terminal load finish/fail -> runtime hook
		std::unordered_map<std::uint64_t, ScriptResultHandler> evalHandlers;  // evalId -> Invoke result cb
		std::uint64_t                                         nextEvalId{ 1 };
		std::unordered_map<std::string, JsListenerHandler>    listenerHandlers;  // "viewId\nname" -> cb
		std::unordered_map<std::string, ConsoleHandler>       consoleHandlers;   // viewId -> cb
		// worker -> game notifications, drained in Update() (guarded by `mutex`):
		std::deque<std::string>                               domReadyNotify;  // viewIds whose DOM became ready
		// terminal load states (finish/fail) routed to the runtime hook; owns its
		// strings since it crosses the worker->game thread boundary.
		struct LoadNotify
		{
			std::string viewId;
			bool        failed{ false };
			std::string url;
			std::string description;
			std::string errorDomain;
			int         errorCode{ 0 };
		};
		std::deque<LoadNotify>                                loadNotify;
		std::deque<ConsoleNotify>                             consoleNotify;
		std::deque<ListenerCall>                              listenerCalls;
		std::deque<std::pair<std::uint64_t, std::string>>     evalResults;     // (evalId, result)
		// game -> worker: views to destroy on the worker thread (guarded by `mutex`):
		std::vector<std::string>                              toDestroy;

		static std::string ListenerKey(std::string_view a_viewId, std::string_view a_name)
		{
			std::string k(a_viewId);
			k.push_back('\n');
			k.append(a_name);
			return k;
		}

		// ---- view lookup ----
		// Active (and currently only) hosted view; null before the first load.
		// Caller holds `mutex` while touching the returned view's shared fields;
		// the pointer itself stays valid for the renderer's lifetime (ViewStates
		// are never erased until the worker tears them down at shutdown, and the
		// unique_ptr keeps the object stable across map rehash).
		[[nodiscard]] ViewState* ActiveView()
		{
			const auto it = views.find(activeViewId);
			return it != views.end() ? it->second.get() : nullptr;
		}
		// Map an Ultralight View back to its ViewState (used by SDK listeners).
		[[nodiscard]] ViewState* FindByView(const ul::View* a_view)
		{
			for (auto& [id, vs] : views) {
				if (vs->view.get() == a_view) {
					return vs.get();
				}
			}
			return nullptr;
		}

		// ================= worker thread =================

		void WorkerMain()
		{
			if (!SetupPlatform()) {
				REX::ERROR("UltralightWebRenderer: worker setup failed; the overlay will never produce frames");
				return;
			}

			// One entry per view, snapshotted under the lock and processed
			// unlocked so the game thread is never blocked behind WebCore.
			struct ViewWork
			{
				ViewState*                                             vs{ nullptr };
				std::optional<ViewManifest>                            load;
				std::optional<std::pair<std::uint32_t, std::uint32_t>> resize;
				std::deque<std::string>                                outbound;
				std::deque<KeyInput>                                   keys;
				std::deque<MouseInput>                                 mice;
				std::deque<EvalRequest>                               eval;
				std::deque<std::pair<std::string, std::string>>      interop;
				std::deque<std::string>                              bind;
			};

			std::unique_lock lock(mutex);
			while (!stopRequested) {
				wake.wait_for(lock, kWorkerFrameInterval);
				if (stopRequested) {
					break;
				}

				if (!toDestroy.empty()) {
					for (const auto& id : toDestroy) {
						if (id == activeViewId) {
							activeViewId.clear();
						}
						drawOrder.erase(std::remove(drawOrder.begin(), drawOrder.end(), id), drawOrder.end());
						views.erase(id);
					}
					toDestroy.clear();
				}
				
				// Snapshot EVERY view's pending work (and the view pointer)
				// under the lock — one entry per view even with no pending work,
				// so each view still gets pumped/harvested below.
				std::vector<ViewWork> work;
				work.reserve(views.size());
				for (auto& [id, vs] : views) {
					ViewWork w;
					w.vs = vs.get();
					w.load = std::exchange(vs->pendingLoad, std::nullopt);
					w.resize = std::exchange(vs->pendingResize, std::nullopt);
					w.outbound.swap(vs->toWeb);
					w.keys.swap(vs->toInput);
					w.mice.swap(vs->toMouse);
					w.eval.swap(vs->toEval);
					w.interop.swap(vs->toInterop);
					w.bind.swap(vs->toBindListener);
					work.push_back(std::move(w));
				}
				// Snapshot the views; sorted into z-order just below.
				std::vector<ViewState*> ordered;
				ordered.reserve(drawOrder.size());
				for (const auto& id : drawOrder) {
					if (const auto it = views.find(id); it != views.end()) {
						ordered.push_back(it->second.get());
					}
				}
				lock.unlock();

				// Lowest zorder first = composited at the bottom. Stable, so views
				// sharing a zorder keep their load order. (zorder is atomic and may
				// change at runtime via SetViewOrder, so reading it unlocked here is
				// safe; that setter flags compositeDirty so a reorder repaints.)
				std::stable_sort(ordered.begin(), ordered.end(),
					[](const ViewState* a, const ViewState* b) { return a->zorder < b->zorder; });

				for (auto& w : work) {
					if (w.load) {
						CreateAndLoadView(*w.vs, *w.load);
					}
					if (w.resize && w.vs->view) {
						w.vs->view->Resize(w.resize->first, w.resize->second);
					}
					for (auto& msg : w.outbound) {
						w.vs->stashedToWeb.push_back(std::move(msg));
					}
					for (const auto& key : w.keys) {
						ProcessKeyInput(*w.vs, key);
					}
					for (const auto& m : w.mice) {
						ProcessMouseInput(*w.vs, m);
					}
				}

				for (auto& w : work) {
					for (const auto& name : w.bind) {
						BindListener(*w.vs, name);
					}
					for (const auto& req : w.eval) {
						DoEval(*w.vs, req);
					}
					for (const auto& ic : w.interop) {
						DoInterop(*w.vs, ic.first, ic.second);
					}
				}
				
				PumpUltralight(ordered);

				lock.lock();
			}
			lock.unlock();

			// Release on this thread — WebCore objects must die where they lived.
			// Clearing the map destroys each ViewState's View ref-pointer here.
			{
				std::scoped_lock release(mutex);
				views.clear();
			}
			session = nullptr;
			renderer = nullptr;
			REX::INFO("UltralightWebRenderer: worker stopped");
		}

		bool SetupPlatform()
		{
			const auto supportDir = config.dataDir / "ultralight";
			auto& platform = ul::Platform::instance();

			REX::INFO("UltralightWebRenderer: worker configuring Ultralight platform");
			ul::Config ulConfig;
			ulConfig.resource_path_prefix = "resources/";  // served by SandboxFileSystem from supportDir
			platform.set_config(ulConfig);

			// Logger/FileSystem are leaked deliberately: Platform keeps raw
			// pointers and Ultralight allows one renderer per process, so
			// these live exactly as long as the process does.
			platform.set_logger(new RexLogger());
			fileSystem = new SandboxFileSystem();
			fileSystem->SetResourceBase(supportDir);
			fileSystem->SetViewsBase(config.dataDir / "views");
			platform.set_file_system(fileSystem);
			// AppCore is linked ONLY for this DirectWrite-backed font loader;
			// no window/app machinery is used.
			REX::INFO("UltralightWebRenderer: worker acquiring platform font loader");
			platform.set_font_loader(ul::GetPlatformFontLoader());

			// Clipboard for in-page copy/cut/paste. Leaked deliberately like the
			// logger above: Platform keeps a raw pointer for the process lifetime
			// (one renderer per process).
			platform.set_clipboard(new WinClipboard());

			REX::INFO("UltralightWebRenderer: worker calling Renderer::Create()");
			renderer = ul::Renderer::Create();
			if (!renderer) {
				REX::ERROR("UltralightWebRenderer: Renderer::Create() failed (check Ultralight log lines above)");
				return false;
			}
			// In-memory session: no cookies/local storage ever touch disk.
			session = renderer->CreateSession(false, "osfui");
			REX::INFO("UltralightWebRenderer: Ultralight renderer created (CPU rendering, in-memory session)");
			return true;
		}

		void CreateAndLoadView(ViewState& a_vs, const ViewManifest& a_manifest)
		{
			ul::ViewConfig viewConfig;
			viewConfig.is_accelerated = false;  // CPU BitmapSurface (Phase 1)
			viewConfig.is_transparent = a_manifest.transparent;
			viewConfig.initial_device_scale = 1.0;
			a_vs.view = renderer->CreateView(a_manifest.width, a_manifest.height, viewConfig, session);
			if (!a_vs.view) {
				REX::ERROR("UltralightWebRenderer: CreateView({}x{}) failed", a_manifest.width, a_manifest.height);
				return;
			}
			a_vs.view->set_load_listener(this);
			a_vs.view->set_view_listener(this);
			a_vs.domReady = false;

			// Folder-qualified URL so the shared SandboxFileSystem can serve
			// several views at once — no per-view "current root" to race.
			const auto folder = a_manifest.rootDir.filename().string();
			const auto url = "file:///" + folder + "/" + a_manifest.entry;
			REX::INFO("UltralightWebRenderer: loading view '{}' -> {} (folder {})",
				a_manifest.id, url, a_manifest.rootDir.string());
			a_vs.view->LoadURL(url.c_str());
		}

		void PumpUltralight(const std::vector<ViewState*>& a_ordered)
		{
			renderer->Update();
			for (auto* vs : a_ordered) {
				if (vs->view && vs->domReady) {
					FlushStashedMessages(*vs);
				}
			}
			renderer->RefreshDisplay(0);
			renderer->Render();  // repaints only the views that need it

			bool anyDirty = false;
			for (auto* vs : a_ordered) {
				if (vs->view) {
					HarvestFrame(*vs);
				}
				anyDirty = anyDirty || vs->backDirty;
			}
			// A hide/show toggles what the composite contains without dirtying any
			// view, so honor a pending recomposite even when nothing repainted.
			const bool forceComposite = compositeDirty.exchange(false);

			// Expose to the game thread. One view: hand its frame straight across
			// (the untouched single-view fast path: per-view triple buffer). More
			// than one: blend them into the composited frame first.
			if (a_ordered.size() <= 1) {
				if (!a_ordered.empty() && a_ordered.front()->backDirty && !a_ordered.front()->hidden.load()) {
					auto* vs = a_ordered.front();
					std::scoped_lock lk(frameMutex);
					std::swap(vs->backFrame, vs->pendingFrame);
					vs->pendingFresh = true;
					vs->backDirty = false;
				}
			} else if (anyDirty || forceComposite) {
				CompositeViews(a_ordered);
				{
					std::scoped_lock lk(frameMutex);
					std::swap(compositedBack, compositedPending);
					compositedFresh = true;
				}
				for (auto* vs : a_ordered) {
					vs->backDirty = false;
				}
			}
		}

		void HarvestFrame(ViewState& a_vs)
		{
			auto* surface = static_cast<ul::BitmapSurface*>(a_vs.view->surface());
			if (!surface || surface->dirty_bounds().IsEmpty()) {
				return;
			}

			const auto stride = surface->row_bytes();
			const auto byteCount = surface->size();
			if (auto* pixels = surface->LockPixels()) {
				a_vs.backFrame.pixels.assign(
					static_cast<const std::uint8_t*>(pixels),
					static_cast<const std::uint8_t*>(pixels) + byteCount);
				a_vs.backFrame.width = surface->width();
				a_vs.backFrame.height = surface->height();
				a_vs.backFrame.strideBytes = stride;
				a_vs.backFrame.index = ++a_vs.paintCount;
				surface->UnlockPixels();
			} else {
				return;
			}
			surface->ClearDirtyBounds();
			a_vs.backDirty = true;

			if (a_vs.paintCount == 1) {
				REX::INFO("UltralightWebRenderer: first paint for view '{}' ({}x{}, stride {})",
					a_vs.id, a_vs.backFrame.width, a_vs.backFrame.height, a_vs.backFrame.strideBytes);
			}
			if (config.devMode && !a_vs.dumpedFirstFrame && a_vs.domReady) {
				// Phase 1 exit criterion: prove real pixels by dumping the
				// first frame painted after DOM ready (earlier paints are a
				// blank pre-style flash). One file: fine while one view is hosted.
				a_vs.dumpedFirstFrame = true;
				const auto dumpPath = (config.dataDir / "ultralight" / "first-frame.png").string();
				if (surface->bitmap()->WritePNG(dumpPath.c_str())) {
					REX::INFO("UltralightWebRenderer: post-DOM frame dumped to {}", dumpPath);
				} else {
					REX::WARN("UltralightWebRenderer: failed to write {}", dumpPath);
				}
			}

			// Exposing this frame to the game thread (single view) or blending it
			// with the others (multi-view) happens in PumpUltralight, once every
			// view has been harvested this pass.
		}

		// Premultiplied-alpha "over" compositing of the hosted views into one
		// tightly-packed BGRA frame (drawOrder = bottom-to-top). Only called when
		// more than one view is hosted; a single view uses the fast path and
		// never lands here. Views are all resized to the output size, so this is
		// a same-size blend; any momentarily mismatched view (mid-resize) is
		// skipped this pass rather than mis-sampled.
		void CompositeViews(const std::vector<ViewState*>& a_ordered)
		{
			std::uint32_t w = 0;
			std::uint32_t h = 0;
			for (const auto* vs : a_ordered) {
				if (!vs->backFrame.pixels.empty()) {
					w = vs->backFrame.width;
					h = vs->backFrame.height;
					break;
				}
			}
			if (w == 0 || h == 0) {
				return;
			}

			const std::uint32_t dstStride = w * 4u;
			compositedBack.pixels.assign(static_cast<std::size_t>(dstStride) * h, 0);  // clear to transparent
			compositedBack.width = w;
			compositedBack.height = h;
			compositedBack.strideBytes = dstStride;

			for (const auto* vs : a_ordered) {
				if (vs->hidden.load()) {
						continue;
				}
				const Frame& src = vs->backFrame;
				if (src.pixels.empty() || src.width != w || src.height != h) {
					continue;
				}
				for (std::uint32_t y = 0; y < h; ++y) {
					std::uint8_t*       d = compositedBack.pixels.data() + static_cast<std::size_t>(y) * dstStride;
					const std::uint8_t* s = src.pixels.data() + static_cast<std::size_t>(y) * src.strideBytes;
					for (std::uint32_t x = 0; x < w; ++x) {
						const std::uint32_t i = x * 4u;
						const unsigned      inv = 255u - s[i + 3];  // 1 - src.alpha (premultiplied "over")
						d[i + 0] = static_cast<std::uint8_t>(s[i + 0] + (d[i + 0] * inv + 127u) / 255u);
						d[i + 1] = static_cast<std::uint8_t>(s[i + 1] + (d[i + 1] * inv + 127u) / 255u);
						d[i + 2] = static_cast<std::uint8_t>(s[i + 2] + (d[i + 2] * inv + 127u) / 255u);
						d[i + 3] = static_cast<std::uint8_t>(s[i + 3] + (d[i + 3] * inv + 127u) / 255u);
					}
				}
			}
			compositedBack.index = ++compositeIndex;
		}

		// ================= JS bridge (worker thread) =================

		// Reads the hidden __viewId string baked onto the osfui object at
		// injection time, identifying which view a postMessage came from.
		[[nodiscard]] static std::string ReadViewId(JSContextRef a_ctx, JSObjectRef a_obj)
		{
			if (!a_obj) {
				return {};
			}
			JSStringRef name = JSStringCreateWithUTF8CString("__viewId");
			JSValueRef  val = JSObjectGetProperty(a_ctx, a_obj, name, nullptr);
			JSStringRelease(name);
			return JSValueIsString(a_ctx, val) ? JSValueToUTF8(a_ctx, val) : std::string{};
		}

		static JSValueRef PostMessageCallback(
			JSContextRef a_ctx, JSObjectRef, JSObjectRef a_this, size_t a_argc,
			const JSValueRef a_args[], JSValueRef*)
		{
			auto* self = sActive.load();
			if (self && a_argc >= 1) {
				// Attribute the message to its SOURCE view (the __viewId on the
				// osfui object). Falls back to the active view only if the
				// call was somehow detached from that object.
				const std::string sourceId = ReadViewId(a_ctx, a_this);
				auto              json = JSValueToUTF8(a_ctx, a_args[0]);
				std::scoped_lock  lk(self->mutex);
				ViewState*        vs = nullptr;
				if (!sourceId.empty()) {
					if (const auto it = self->views.find(sourceId); it != self->views.end()) {
						vs = it->second.get();
					}
				}
				if (!vs) {
					vs = self->ActiveView();
				}
				if (vs) {
					if (vs->toNative.size() < kMaxQueuedMessages) {
						vs->toNative.push_back(std::move(json));
					} else {
						static std::once_flag once;
						Log::WarnOnce(once, "UltralightWebRenderer: web->native queue full; dropping messages");
					}
				}
			}
			return JSValueMakeUndefined(a_ctx);
		}

		// window.osfui.__invokeListener(name, arg): queue a (viewId,name,arg)
		// ListenerCall, drained on the game thread in Update(). Mirrors
		// PostMessageCallback's __viewId source attribution.
		static JSValueRef InvokeListenerCallback(
			JSContextRef a_ctx, JSObjectRef, JSObjectRef a_this, size_t a_argc,
			const JSValueRef a_args[], JSValueRef*)
		{
			auto* self = sActive.load();
			if (self && a_argc >= 2) {
				const std::string sourceId = ReadViewId(a_ctx, a_this);
				auto              name = JSValueToUTF8(a_ctx, a_args[0]);
				auto              arg = JSValueToUTF8(a_ctx, a_args[1]);
				std::scoped_lock  lk(self->mutex);
				if (self->listenerCalls.size() < kMaxQueuedMessages) {
					self->listenerCalls.push_back(ListenerCall{ sourceId, std::move(name), std::move(arg) });
				}
			}
			return JSValueMakeUndefined(a_ctx);
		}

		void InjectBridge(ViewState& a_vs)
		{
			auto scopedCtx = a_vs.view->LockJSContext();
			JSContextRef ctx = scopedCtx->ctx();
			JSObjectRef  global = JSContextGetGlobalObject(ctx);

			// window.osfui = { postMessage: <native fn> }
			// The object stays extensible so the page can attach onMessage;
			// postMessage itself is read-only.
			JSObjectRef osfui = JSObjectMake(ctx, nullptr, nullptr);
			JSStringRef postName = JSStringCreateWithUTF8CString("postMessage");
			JSObjectRef postFn = JSObjectMakeFunctionWithCallback(ctx, postName, &PostMessageCallback);
			JSObjectSetProperty(ctx, osfui, postName, postFn,
				kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete, nullptr);
			JSStringRelease(postName);

			// Bake the source view id so PostMessageCallback can attribute this
			// view's messages. Read-only + non-enumerable so the page can't spoof
			// or even see it.
			JSStringRef vidName = JSStringCreateWithUTF8CString("__viewId");
			JSStringRef vidStr = JSStringCreateWithUTF8CString(a_vs.id.c_str());
			JSObjectSetProperty(ctx, osfui, vidName, JSValueMakeString(ctx, vidStr),
				kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete, nullptr);
			JSStringRelease(vidStr);
			JSStringRelease(vidName);

			// window.osfui.__invokeListener(name, arg): the JS->native path for
			// RegisterJsFunction. The bound window.<name> wrappers call it; native
			// attributes it via the baked __viewId.
			JSStringRef invName = JSStringCreateWithUTF8CString("__invokeListener");
			JSObjectRef invFn = JSObjectMakeFunctionWithCallback(ctx, invName, &InvokeListenerCallback);
			JSObjectSetProperty(ctx, osfui, invName, invFn,
				kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete, nullptr);
			JSStringRelease(invName);

			JSStringRef osfuiName = JSStringCreateWithUTF8CString("osfui");
			JSObjectSetProperty(ctx, global, osfuiName, osfui, kJSPropertyAttributeNone, nullptr);
			JSStringRelease(osfuiName);
		}

		void FlushStashedMessages(ViewState& a_vs)
		{
			while (!a_vs.stashedToWeb.empty()) {
				const auto json = std::move(a_vs.stashedToWeb.front());
				a_vs.stashedToWeb.pop_front();
				DeliverToWeb(a_vs, json);
			}
		}

		void DeliverToWeb(ViewState& a_vs, const std::string& a_json)
		{
			auto scopedCtx = a_vs.view->LockJSContext();
			JSContextRef ctx = scopedCtx->ctx();
			JSObjectRef  global = JSContextGetGlobalObject(ctx);

			JSStringRef osfuiName = JSStringCreateWithUTF8CString("osfui");
			JSValueRef  osfuiVal = JSObjectGetProperty(ctx, global, osfuiName, nullptr);
			JSStringRelease(osfuiName);
			if (!JSValueIsObject(ctx, osfuiVal)) {
				REX::WARN("UltralightWebRenderer: window.osfui missing; dropped native->web message");
				return;
			}
			JSObjectRef osfui = JSValueToObject(ctx, osfuiVal, nullptr);

			JSStringRef onName = JSStringCreateWithUTF8CString("onMessage");
			JSValueRef  onVal = JSObjectGetProperty(ctx, osfui, onName, nullptr);
			JSStringRelease(onName);
			if (!JSValueIsObject(ctx, onVal) || !JSObjectIsFunction(ctx, JSValueToObject(ctx, onVal, nullptr))) {
				REX::WARN("UltralightWebRenderer: window.osfui.onMessage is not a function; dropped native->web message");
				return;
			}

			JSStringRef jsonStr = JSStringCreateWithUTF8CString(a_json.c_str());
			JSValueRef  arg = JSValueMakeString(ctx, jsonStr);
			JSStringRelease(jsonStr);
			JSValueRef exception = nullptr;
			JSObjectCallAsFunction(ctx, JSValueToObject(ctx, onVal, nullptr), osfui, 1, &arg, &exception);
			if (exception) {
				REX::WARN("UltralightWebRenderer: onMessage threw: {}",
					JSValueToUTF8(ctx, exception).substr(0, kMaxLoggedTextLen));
			}
		}

		// ---- per-view JS ops (worker thread) ----

		// Bind window.<name>(arg) as a thin wrapper calling __invokeListener.
		void BindListener(ViewState& a_vs, const std::string& a_name)
		{
			if (!a_vs.view) {
				return;
			}
			std::string esc;
			for (char c : a_name) {
				if (c == '\\' || c == '"') {
					esc.push_back('\\');
				}
				esc.push_back(c);
			}
			const std::string js =
				"window[\"" + esc + "\"]=function(a){if(window.osfui&&window.osfui.__invokeListener)"
				"window.osfui.__invokeListener(\"" + esc + "\",a===undefined?\"\":String(a));};";
			auto         scopedCtx = a_vs.view->LockJSContext();
			JSContextRef ctx = scopedCtx->ctx();
			JSStringRef  s = JSStringCreateWithUTF8CString(js.c_str());
			JSValueRef   exc = nullptr;
			JSEvaluateScript(ctx, s, nullptr, nullptr, 0, &exc);
			JSStringRelease(s);
			if (exc) {
				REX::WARN("UltralightWebRenderer: RegisterJSListener bind of '{}' threw", a_name);
			}
		}

		// Run arbitrary JS; push the result back to the game thread if wanted.
		void DoEval(ViewState& a_vs, const EvalRequest& a_req)
		{
			std::string out;
			if (a_vs.view) {
				auto         scopedCtx = a_vs.view->LockJSContext();
				JSContextRef ctx = scopedCtx->ctx();
				JSStringRef  s = JSStringCreateWithUTF8CString(a_req.script.c_str());
				JSValueRef   exc = nullptr;
				JSValueRef   res = JSEvaluateScript(ctx, s, nullptr, nullptr, 0, &exc);
				JSStringRelease(s);
				if (exc) {
					REX::WARN("UltralightWebRenderer: Invoke threw: {}",
						JSValueToUTF8(ctx, exc).substr(0, kMaxLoggedTextLen));
				} else if (res && !JSValueIsUndefined(ctx, res) && !JSValueIsNull(ctx, res)) {
					out = JSValueToUTF8(ctx, res);
				}
			}
			if (a_req.evalId != 0) {
				std::scoped_lock lk(mutex);
				evalResults.emplace_back(a_req.evalId, std::move(out));
			}
		}

		// Call window.<fn>(arg) directly (no eval/parse).
		void DoInterop(ViewState& a_vs, const std::string& a_fn, const std::string& a_arg)
		{
			if (!a_vs.view) {
				return;
			}
			auto         scopedCtx = a_vs.view->LockJSContext();
			JSContextRef ctx = scopedCtx->ctx();
			JSObjectRef  global = JSContextGetGlobalObject(ctx);
			JSStringRef  fnName = JSStringCreateWithUTF8CString(a_fn.c_str());
			JSValueRef   fnVal = JSObjectGetProperty(ctx, global, fnName, nullptr);
			JSStringRelease(fnName);
			if (!JSValueIsObject(ctx, fnVal)) {
				return;
			}
			JSObjectRef fnObj = JSValueToObject(ctx, fnVal, nullptr);
			if (!fnObj || !JSObjectIsFunction(ctx, fnObj)) {
				return;
			}
			JSStringRef argStr = JSStringCreateWithUTF8CString(a_arg.c_str());
			JSValueRef  arg = JSValueMakeString(ctx, argStr);
			JSStringRelease(argStr);
			JSValueRef exc = nullptr;
			JSObjectCallAsFunction(ctx, fnObj, global, 1, &arg, &exc);
			if (exc) {
				REX::WARN("UltralightWebRenderer: InteropCall '{}' threw", a_fn);
			}
		}

		// ================= keyboard input (worker thread) =================

		[[nodiscard]] static unsigned CurrentModifiers(const ViewState& a_vs)
		{
			unsigned m = 0;
			if (a_vs.modShift) {
				m |= ul::KeyEvent::kMod_ShiftKey;
			}
			if (a_vs.modCtrl) {
				m |= ul::KeyEvent::kMod_CtrlKey;
			}
			if (a_vs.modAlt) {
				m |= ul::KeyEvent::kMod_AltKey;
			}
			return m;
		}

		void ProcessKeyInput(ViewState& a_vs, const KeyInput& a_key)
		{
			if (!a_vs.view) {
				return;
			}

			// Char events carry a finished Unicode scalar value from the OS
			// WM_CHAR/WM_UNICHAR stream — already resolved for the active layout,
			// dead keys, and AltGr. This is the sole source of text entry; the VK
			// path below no longer synthesizes one. The matching RawKeyDown was
			// queued ahead of this on the same FIFO, so ordering is correct.
			if (a_key.kind == KeyInput::Kind::kChar) {
				const auto utf8 = CodepointToUTF8(a_key.codepoint);
				if (utf8.empty()) {
					return;
				}
				const ul::String text(utf8.c_str());
				ul::KeyEvent ch;
				ch.type = ul::KeyEvent::kType_Char;
				// Carry only Shift: Windows already baked casing and AltGr into
				// the codepoint, and tagging it Ctrl/Alt (AltGr holds both) would
				// make WebCore treat the keypress as a command instead of text.
				ch.modifiers = a_vs.modShift ? ul::KeyEvent::kMod_ShiftKey : 0u;
				ch.text = text;
				ch.unmodified_text = text;
				a_vs.view->FireKeyEvent(ch);
				return;
			}

			// Track modifier state from the VK stream (Windows VK_* codes).
			switch (a_key.vk) {
			case 0x10:           // VK_SHIFT
			case 0xA0: case 0xA1:  // VK_LSHIFT / VK_RSHIFT
				a_vs.modShift = a_key.down;
				break;
			case 0x11:           // VK_CONTROL
			case 0xA2: case 0xA3:  // VK_LCONTROL / VK_RCONTROL
				a_vs.modCtrl = a_key.down;
				break;
			case 0x12:           // VK_MENU (Alt)
			case 0xA4: case 0xA5:  // VK_LMENU / VK_RMENU
				a_vs.modAlt = a_key.down;
				break;
			default:
				break;
			}

			if (!a_vs.loggedFirstKey) {
				a_vs.loggedFirstKey = true;
				REX::INFO("UltralightWebRenderer: first key routed into view '{}' (vk={:#x}, down={})",
					a_vs.id, a_key.vk, a_key.down);
			}

			ul::KeyEvent key;
			key.virtual_key_code = static_cast<int>(a_key.vk);
			key.native_key_code = static_cast<int>(a_key.vk);
			key.modifiers = CurrentModifiers(a_vs);
			key.is_keypad = false;
			key.is_auto_repeat = false;
			key.is_system_key = false;
			ul::GetKeyIdentifierFromVirtualKeyCode(key.virtual_key_code, key.key_identifier);

			// The unmodified key text (the key value with Ctrl/Alt ignored) is how
			// WebCore resolves accelerator shortcuts — Ctrl+A/C/V/X/Z — and it must
			// ride on the RawKeyDown itself. Without it every Ctrl-shortcut (and
			// thus clipboard) is dead. Text *entry* is handled separately by the
			// Char events fed from the OS WM_CHAR stream, so none is emitted here.
			ul::String keyText;
			ul::GetKeyFromVirtualKeyCode(key.virtual_key_code, a_vs.modShift, keyText);
			const auto keyUtf8 = ToUTF8(keyText);
			const bool printable = keyUtf8.size() == 1 && static_cast<unsigned char>(keyUtf8[0]) >= 0x20;

			if (a_key.down) {
				key.type = ul::KeyEvent::kType_RawKeyDown;
				if (printable) {
					key.text = keyText;
					key.unmodified_text = keyText;
				}
				a_vs.view->FireKeyEvent(key);
			} else {
				key.type = ul::KeyEvent::kType_KeyUp;
				a_vs.view->FireKeyEvent(key);
			}
		}

		// ================= mouse input (worker thread) =================

		void ProcessMouseInput(ViewState& a_vs, const MouseInput& a_m)
		{
			if (!a_vs.view) {
				return;
			}
			if (a_m.kind == MouseInput::Kind::kScroll) {
				// Notches -> pixels via the per-view scroll step
				// (SetScrollPixelSize). Multiply before divide so sub-notch
				// high-resolution wheels still move. Positive delta = scroll up.
				// Ultralight scrolls whatever the last MouseMoved hovered; the
				// shared toMouse deque is processed in order, so the hover target
				// is already current here — no extra MouseMoved needed.
				const int pixels = (a_m.wheelDelta * a_vs.scrollPx.load()) / kWheelDelta;
				ul::ScrollEvent ev;
				ev.type = ul::ScrollEvent::kType_ScrollByPixel;
				ev.delta_x = 0;
				ev.delta_y = pixels;
				a_vs.view->FireScrollEvent(ev);
				return;
			}
			ul::MouseEvent ev;
			ev.x = a_m.x;
			ev.y = a_m.y;
			switch (a_m.kind) {
			case MouseInput::Kind::kMove:
				ev.type = ul::MouseEvent::kType_MouseMoved;
				ev.button = ul::MouseEvent::kButton_None;
				break;
			case MouseInput::Kind::kDown:
				ev.type = ul::MouseEvent::kType_MouseDown;
				ev.button = ToUlButton(a_m.button);
				break;
			case MouseInput::Kind::kUp:
				ev.type = ul::MouseEvent::kType_MouseUp;
				ev.button = ToUlButton(a_m.button);
				break;
			case MouseInput::Kind::kScroll:
				return;  // handled above; case present only to keep the switch exhaustive
			}
			a_vs.view->FireMouseEvent(ev);
		}

		[[nodiscard]] static ul::MouseEvent::Button ToUlButton(int a_button)
		{
			switch (a_button) {  // MouseButton order: 0=left, 1=right, 2=middle
			case 1:
				return ul::MouseEvent::kButton_Right;
			case 2:
				return ul::MouseEvent::kButton_Middle;
			default:
				return ul::MouseEvent::kButton_Left;
			}
		}

		// ================= Ultralight listeners (worker thread) =================

		// SDK listeners fire on the worker thread during renderer->Update()/
		// Render() (PumpUltralight runs unlocked), so taking `mutex` to resolve
		// the source view is safe and non-recursive. The resolved pointer stays
		// valid afterwards (ViewStates aren't erased before shutdown).
		void OnWindowObjectReady(ul::View* a_view, std::uint64_t, bool a_isMainFrame, const ul::String&) override
		{
			if (!a_isMainFrame) {
				return;
			}
			ViewState* vs = nullptr;
			{
				std::scoped_lock lk(mutex);
				vs = FindByView(a_view);
			}
			// Only views that requested it (manifest permissions.nativeBridge)
			// get window.osfui at all.
			if (vs && vs->bridgeAllowed) {
				InjectBridge(*vs);
			}
		}

		void OnDOMReady(ul::View* a_view, std::uint64_t, bool a_isMainFrame, const ul::String& a_url) override
		{
			if (!a_isMainFrame) {
				return;
			}
			ViewState* vs = nullptr;
			{
				std::scoped_lock lk(mutex);
				vs = FindByView(a_view);
			}
			if (!vs) {
				return;
			}
			vs->domReady = true;
			REX::INFO("UltralightWebRenderer: DOM ready for view '{}' ({})", vs->id, ToUTF8(a_url));
			{
				std::scoped_lock lk(mutex);
				if (domReadyNotify.size() < kMaxQueuedMessages) {
					domReadyNotify.push_back(vs->id);
				}
			}
			// Give the view input focus so routed key events reach the focused
			// DOM element (the page autofocuses its input).
			vs->view->Focus();
			FlushStashedMessages(*vs);
		}

		void OnFinishLoading(ul::View* a_view, std::uint64_t, bool a_isMainFrame, const ul::String& a_url) override
		{
			if (!a_isMainFrame) {
				return;
			}
			std::string id;
			{
				std::scoped_lock lk(mutex);
				if (const ViewState* vs = FindByView(a_view)) {
					id = vs->id;
				}
			}
			if (id.empty()) {
				return;
			}
			LoadNotify n{ .viewId = std::move(id), .failed = false, .url = ToUTF8(a_url) };
			std::scoped_lock lk(mutex);
			if (loadNotify.size() < kMaxQueuedMessages) {
				loadNotify.push_back(std::move(n));
			}
		}

		void OnFailLoading(ul::View* a_view, std::uint64_t, bool a_isMainFrame, const ul::String& a_url,
			const ul::String& a_description, const ul::String& a_errorDomain, int a_errorCode) override
		{
			REX::ERROR("UltralightWebRenderer: load failed (mainFrame={}, url={}): {} [{} {}]",
				a_isMainFrame, ToUTF8(a_url), ToUTF8(a_description), ToUTF8(a_errorDomain), a_errorCode);
			if (!a_isMainFrame) {
				return;
			}
			std::string id;
			{
				std::scoped_lock lk(mutex);
				if (const ViewState* vs = FindByView(a_view)) {
					id = vs->id;
				}
			}
			if (id.empty()) {
				return;
			}
			LoadNotify n{ .viewId = std::move(id), .failed = true, .url = ToUTF8(a_url),
				.description = ToUTF8(a_description), .errorDomain = ToUTF8(a_errorDomain), .errorCode = a_errorCode };
			std::scoped_lock lk(mutex);
			if (loadNotify.size() < kMaxQueuedMessages) {
				loadNotify.push_back(std::move(n));
			}
		}

		void OnAddConsoleMessage(ul::View* a_view, const ul::ConsoleMessage& a_message) override
		{
			const auto text = ToUTF8(a_message.message()).substr(0, kMaxLoggedTextLen);
			
			{
				std::scoped_lock lk(mutex);
				if (ViewState* vs = FindByView(a_view); vs && vs->wantsConsole.load()) {
					int lvl = 0;
					if (a_message.level() == ul::kMessageLevel_Warning) {
						lvl = 1;
					} else if (a_message.level() == ul::kMessageLevel_Error) {
						lvl = 2;
					}
					if (consoleNotify.size() < kMaxQueuedMessages) {
						consoleNotify.push_back(ConsoleNotify{ vs->id, lvl, ToUTF8(a_message.message()) });
					}
				}
			}
			switch (a_message.level()) {
			case ul::kMessageLevel_Error:
				REX::ERROR("UltralightWebRenderer: [console] {} ({}:{})", text,
					ToUTF8(a_message.source_id()), a_message.line_number());
				break;
			case ul::kMessageLevel_Warning:
				REX::WARN("UltralightWebRenderer: [console] {}", text);
				break;
			default:
				REX::DEBUG("UltralightWebRenderer: [console] {}", text);
				break;
			}
		}
	};

	// ================= game-thread interface =================

	UltralightWebRenderer::UltralightWebRenderer() :
		_impl(std::make_unique<Impl>())
	{}

	UltralightWebRenderer::~UltralightWebRenderer()
	{
		Shutdown();
	}

	bool UltralightWebRenderer::PreloadRuntime(const std::filesystem::path& a_dataDir)
	{
		// NOTE: this function must not touch any Ultralight symbol — it runs
		// before the SDK DLLs are in the process.
		const auto supportDir = a_dataDir / "ultralight";
		const auto binDir = supportDir / "bin";

		// SFSE loads plugins with plain LoadLibrary, so the Ultralight DLLs
		// (delay-loaded imports of this plugin) would never resolve from our
		// folder on their own. Preload them explicitly, dependencies first;
		// later delay-load resolution then finds them by module name.
		for (const auto* name : { L"UltralightCore.dll", L"WebCore.dll", L"Ultralight.dll", L"AppCore.dll" }) {
			const auto dllPath = binDir / name;
			std::error_code ec;
			if (!std::filesystem::exists(dllPath, ec)) {
				REX::ERROR("UltralightWebRenderer: missing {} — was the plugin deployed from a with_ultralight build?",
					dllPath.string());
				return false;
			}
			if (std::uint32_t lastError = 0; !Platform::LoadLibraryAbsolute(dllPath, lastError)) {
				REX::ERROR("UltralightWebRenderer: failed to load {} (Win32 error {})", dllPath.string(), lastError);
				return false;
			}
		}

		std::error_code ec;
		if (!std::filesystem::exists(supportDir / "resources" / "icudt67l.dat", ec)) {
			REX::ERROR("UltralightWebRenderer: missing {} (required ICU data)",
				(supportDir / "resources" / "icudt67l.dat").string());
			return false;
		}
		REX::INFO("UltralightWebRenderer: SDK DLLs preloaded from {}", binDir.string());
		return true;
	}

	bool UltralightWebRenderer::Initialize(const RendererConfig& a_config)
	{
		_impl->config = a_config;
		Impl::sActive.store(_impl.get());
		// The worker thread (and all WebCore machinery) starts lazily on the
		// first Update() tick: SFSE's plugin-load phase runs in a fragile
		// pre-main process state (3 threads, usvfs hooks mid-bootstrap) where
		// heavyweight DLL init proved to hang. By the first tick the game is
		// fully alive.
		REX::INFO("UltralightWebRenderer: initialized ({}x{}, CPU BitmapSurface; worker starts on first tick)",
			a_config.width, a_config.height);
		return true;
	}

	void UltralightWebRenderer::Shutdown()
	{
		if (!_impl || !_impl->worker.joinable()) {
			return;
		}
		{
			std::scoped_lock lk(_impl->mutex);
			_impl->stopRequested = true;
		}
		_impl->wake.notify_all();
		_impl->worker.join();
		Impl::sActive.store(nullptr);
	}

	void UltralightWebRenderer::LoadView(const ViewManifest& a_manifest)
	{
		{
			std::scoped_lock lk(_impl->mutex);
			// Create-or-update the view entry for this id (additive: other views
			// stay loaded). The game thread owns the map slot; the worker creates
			// the actual ul::View when it picks up pendingLoad. New ids append to
			// drawOrder (= z-order, bottom-to-top). The first view loaded becomes
			// active by default; call SetActiveView to choose another.
			auto&      slot = _impl->views[a_manifest.id];
			const bool isNew = !slot;
			if (!slot) {
				slot = std::make_unique<Impl::ViewState>();
			}
			slot->id = a_manifest.id;
			slot->zorder = a_manifest.zorder;
			slot->interactive = a_manifest.interactive;
			slot->bridgeAllowed = a_manifest.permissions.nativeBridge;
			slot->pendingLoad = a_manifest;
			if (isNew) {
				_impl->drawOrder.push_back(a_manifest.id);
			}
			// First INTERACTIVE view becomes active by default; a passive view
			// (e.g. a HUD) never auto-focuses. SetActiveView can override.
			if (_impl->activeViewId.empty() && a_manifest.interactive) {
				_impl->activeViewId = a_manifest.id;
			}
		}
		_impl->wake.notify_all();
	}

	void UltralightWebRenderer::SetActiveView(std::string_view a_id)
	{
		std::scoped_lock lk(_impl->mutex);
		const auto it = _impl->views.find(std::string(a_id));
		if (it == _impl->views.end()) {
			REX::WARN("UltralightWebRenderer: SetActiveView('{}') ignored — view not loaded (active stays '{}')",
				a_id, _impl->activeViewId);
			return;
		}
		if (!it->second->interactive) {
			REX::WARN("UltralightWebRenderer: SetActiveView('{}') ignored — view is not interactive (active stays '{}')",
				a_id, _impl->activeViewId);
			return;
		}
		_impl->activeViewId = std::string(a_id);
	}

	void UltralightWebRenderer::Resize(std::uint32_t a_width, std::uint32_t a_height)
	{
		{
			std::scoped_lock lk(_impl->mutex);
			// Resize every hosted view to the output size so their frames
			// composite 1:1 (single view: this is just the one view).
			for (auto& [id, vs] : _impl->views) {
				vs->pendingResize = { a_width, a_height };
			}
		}
		_impl->wake.notify_all();
	}

	void UltralightWebRenderer::Update(double)
	{
		// Lazy worker start (see Initialize for why). First tick only.
		if (!_impl->worker.joinable()) {
			REX::INFO("UltralightWebRenderer: starting worker thread");
			_impl->worker = std::thread(&Impl::WorkerMain, _impl.get());
		}

		// The worker self-paces; our job here is only to hand page->game messages
		// to the runtime on the game thread, each tagged with its source view.
		std::vector<std::pair<std::string, std::string>> inbound;  // (viewId, json)
		{
			std::scoped_lock lk(_impl->mutex);
			for (auto& [id, vs] : _impl->views) {
				while (!vs->toNative.empty()) {
					inbound.emplace_back(id, std::move(vs->toNative.front()));
					vs->toNative.pop_front();
				}
			}
		}
		std::deque<std::string>                            apiDomReady;
		std::deque<Impl::ListenerCall>                     apiListeners;
		std::deque<Impl::ConsoleNotify>                    apiConsoles;
		std::deque<std::pair<std::uint64_t, std::string>> apiResults;
		std::deque<Impl::LoadNotify>                       apiLoads;
		{
			std::scoped_lock lk(_impl->mutex);
			apiDomReady.swap(_impl->domReadyNotify);
			apiListeners.swap(_impl->listenerCalls);
			apiConsoles.swap(_impl->consoleNotify);
			apiResults.swap(_impl->evalResults);
			apiLoads.swap(_impl->loadNotify);
		}
		if (_impl->onDomReady) {
			for (const auto& id : apiDomReady) {
				_impl->onDomReady(id);
			}
		}
		// String views below point into apiLoads, which outlives the calls.
		if (_impl->onLoad) {
			for (const auto& n : apiLoads) {
				_impl->onLoad(LoadEvent{ .viewId = n.viewId, .failed = n.failed, .url = n.url,
					.description = n.description, .errorDomain = n.errorDomain, .errorCode = n.errorCode });
			}
		}
		for (const auto& lc : apiListeners) {
			const auto hit = _impl->listenerHandlers.find(Impl::ListenerKey(lc.viewId, lc.name));
			if (hit != _impl->listenerHandlers.end() && hit->second) {
				hit->second(lc.arg);
			}
		}
		for (const auto& cn : apiConsoles) {
			const auto cit = _impl->consoleHandlers.find(cn.viewId);
			if (cit != _impl->consoleHandlers.end() && cit->second) {
				cit->second(cn.level, cn.message);
			}
		}
		for (auto& pr : apiResults) {
			const auto eit = _impl->evalHandlers.find(pr.first);
			if (eit != _impl->evalHandlers.end()) {
				if (eit->second) {
					eit->second(pr.second);
				}
				_impl->evalHandlers.erase(eit);
			}
		}
		
		if (_impl->onWebMessage) {
			for (const auto& [viewId, json] : inbound) {
				_impl->onWebMessage(viewId, json);
			}
		}
	}

	std::optional<FrameBufferView> UltralightWebRenderer::Render()
	{
		// Decide single vs composited output (and, for single, resolve the active
		// view) under `mutex`. Then touch frames under `frameMutex` only — never
		// both held at once. The ViewState pointer stays valid for the renderer's
		// lifetime; front/compositedFront are game-thread-only.
		bool             multi = false;
		Impl::ViewState* active = nullptr;
		{
			std::scoped_lock lk(_impl->mutex);
			multi = _impl->drawOrder.size() > 1;
			active = _impl->ActiveView();
		}

		// Multi-view: return the worker's composited frame.
		if (multi) {
			std::scoped_lock lk(_impl->frameMutex);
			if (_impl->compositedFresh) {
				std::swap(_impl->compositedPending, _impl->compositedFront);
				_impl->compositedFresh = false;
			}
			const auto& front = _impl->compositedFront;
			if (front.pixels.empty()) {
				return std::nullopt;
			}
			return FrameBufferView{
				.pixels = front.pixels,
				.width = front.width,
				.height = front.height,
				.strideBytes = front.strideBytes,
				.format = PixelFormat::kBGRA8,
				.frameIndex = front.index,
			};
		}

		// Single-view fast path (unchanged): the active view's own frame.
		if (!active) {
			return std::nullopt;
		}
		{
			std::scoped_lock lk(_impl->frameMutex);
			if (active->pendingFresh) {
				std::swap(active->pendingFrame, active->frontFrame);
				active->pendingFresh = false;
			}
		}
		const auto& front = active->frontFrame;
		if (front.pixels.empty()) {
			return std::nullopt;
		}
		return FrameBufferView{
			.pixels = front.pixels,
			.width = front.width,
			.height = front.height,
			.strideBytes = front.strideBytes,
			.format = PixelFormat::kBGRA8,
			.frameIndex = front.index,
		};
	}

	void UltralightWebRenderer::SendMessageToWeb(std::string_view a_viewId, std::string_view a_json)
	{
		{
			std::scoped_lock lk(_impl->mutex);
			const auto it = _impl->views.find(std::string(a_viewId));
			if (it == _impl->views.end()) {
				return;  // unknown/unloaded view
			}
			auto* vs = it->second.get();
			if (vs->toWeb.size() >= kMaxQueuedMessages) {
				static std::once_flag once;
				Log::WarnOnce(once, "UltralightWebRenderer: native->web queue full; dropping messages");
				return;
			}
			vs->toWeb.emplace_back(a_json);
		}
		_impl->wake.notify_all();
	}

	void UltralightWebRenderer::SetWebMessageHandler(WebMessageHandler a_handler)
	{
		_impl->onWebMessage = std::move(a_handler);
	}

	void UltralightWebRenderer::SetDomReadyHandler(DomReadyHandler a_handler)
	{
		_impl->onDomReady = std::move(a_handler);
	}

	void UltralightWebRenderer::SetLoadHandler(LoadHandler a_handler)
	{
		_impl->onLoad = std::move(a_handler);
	}

	void UltralightWebRenderer::EvaluateScript(std::string_view a_viewId, std::string_view a_js, ScriptResultHandler a_onResult)
	{
		std::scoped_lock lk(_impl->mutex);
		const auto it = _impl->views.find(std::string(a_viewId));
		if (it == _impl->views.end()) {
			return;
		}
		std::uint64_t evalId = 0;
		if (a_onResult) {
			evalId = _impl->nextEvalId++;
			_impl->evalHandlers.emplace(evalId, std::move(a_onResult));
		}
		it->second->toEval.push_back(Impl::EvalRequest{ evalId, std::string(a_js) });
		_impl->wake.notify_all();
	}

	void UltralightWebRenderer::CallJsFunction(std::string_view a_viewId, std::string_view a_fnName, std::string_view a_arg)
	{
		{
			std::scoped_lock lk(_impl->mutex);
			const auto it = _impl->views.find(std::string(a_viewId));
			if (it == _impl->views.end()) {
				return;
			}
			it->second->toInterop.emplace_back(std::string(a_fnName), std::string(a_arg));
		}
		_impl->wake.notify_all();
	}

	void UltralightWebRenderer::RegisterJsFunction(std::string_view a_viewId, std::string_view a_name, JsListenerHandler a_callback)
	{
		std::scoped_lock lk(_impl->mutex);
		const auto it = _impl->views.find(std::string(a_viewId));
		if (it == _impl->views.end()) {
			return;
		}
		_impl->listenerHandlers[Impl::ListenerKey(a_viewId, a_name)] = std::move(a_callback);
		it->second->toBindListener.emplace_back(a_name);
		_impl->wake.notify_all();
	}

	void UltralightWebRenderer::SetConsoleHandler(std::string_view a_viewId, ConsoleHandler a_handler)
	{
		std::scoped_lock lk(_impl->mutex);
		const auto it = _impl->views.find(std::string(a_viewId));
		if (it == _impl->views.end()) {
			return;
		}
		const bool has = static_cast<bool>(a_handler);
		if (has) {
			_impl->consoleHandlers[std::string(a_viewId)] = std::move(a_handler);
		} else {
			_impl->consoleHandlers.erase(std::string(a_viewId));
		}
		it->second->wantsConsole.store(has);
	}

	void UltralightWebRenderer::SetViewHidden(std::string_view a_viewId, bool a_hidden)
	{
		std::scoped_lock lk(_impl->mutex);
		if (const auto it = _impl->views.find(std::string(a_viewId)); it != _impl->views.end()) {
			it->second->hidden.store(a_hidden);
			// Recompose even if no view repaints, else the hidden one lingers.
			_impl->compositeDirty.store(true);
		}
		_impl->wake.notify_all();
	}

	void UltralightWebRenderer::SetViewOrder(std::string_view a_viewId, int a_order)
	{
		std::scoped_lock lk(_impl->mutex);
		if (const auto it = _impl->views.find(std::string(a_viewId)); it != _impl->views.end()) {
			it->second->zorder.store(a_order);
			// Recompose even if no view repaints, else the reorder of two static
			// views doesn't show on screen (same gap fixed for SetViewHidden).
			_impl->compositeDirty.store(true);
		}
		_impl->wake.notify_all();
	}

	void UltralightWebRenderer::SetScrollPixelSize(std::string_view a_viewId, int a_pixels)
	{
		std::scoped_lock lk(_impl->mutex);
		if (const auto it = _impl->views.find(std::string(a_viewId)); it != _impl->views.end()) {
			it->second->scrollPx.store(a_pixels);
		}
	}

	void UltralightWebRenderer::DestroyView(std::string_view a_viewId)
	{
		{
			std::scoped_lock lk(_impl->mutex);
			const std::string id(a_viewId);
			if (_impl->views.find(id) == _impl->views.end()) {
				return;
			}
			_impl->toDestroy.push_back(id);
			_impl->consoleHandlers.erase(id);
			const std::string prefix = id + "\n";
			for (auto hit = _impl->listenerHandlers.begin(); hit != _impl->listenerHandlers.end();) {
				if (hit->first.rfind(prefix, 0) == 0) {
					hit = _impl->listenerHandlers.erase(hit);
				} else {
					++hit;
				}
			}
		}
		_impl->wake.notify_all();
	}

	void UltralightWebRenderer::InjectKeyEvent(std::uint32_t a_vkCode, bool a_down)
	{
		{
			std::scoped_lock lk(_impl->mutex);
			if (auto* vs = _impl->ActiveView()) {
				// Bound the queue so a stuck worker can't accumulate unbounded
				// keystrokes; dropping the oldest keeps the newest responsive.
				if (vs->toInput.size() >= kMaxQueuedMessages) {
					vs->toInput.pop_front();
				}
				vs->toInput.push_back(Impl::KeyInput{
					.kind = Impl::KeyInput::Kind::kVirtualKey, .vk = a_vkCode, .down = a_down });
			}
		}
		_impl->wake.notify_all();
	}

	void UltralightWebRenderer::InjectCharEvent(std::uint32_t a_codepoint)
	{
		{
			std::scoped_lock lk(_impl->mutex);
			if (auto* vs = _impl->ActiveView()) {
				// Shares the keyboard FIFO with the VK stream so this Char stays
				// ordered after the RawKeyDown that produced it (WebCore depends
				// on keydown-before-keypress for text insertion).
				if (vs->toInput.size() >= kMaxQueuedMessages) {
					vs->toInput.pop_front();
				}
				vs->toInput.push_back(Impl::KeyInput{
					.kind = Impl::KeyInput::Kind::kChar, .codepoint = a_codepoint });
			}
		}
		_impl->wake.notify_all();
	}

	void UltralightWebRenderer::InjectMouseMove(int a_x, int a_y)
	{
		{
			std::scoped_lock lk(_impl->mutex);
			if (auto* vs = _impl->ActiveView()) {
				// Coalesce: only the latest position matters, so collapse runs of
				// pending moves into one to keep the queue (and worker) light.
				if (!vs->toMouse.empty() && vs->toMouse.back().kind == Impl::MouseInput::Kind::kMove) {
					vs->toMouse.back().x = a_x;
					vs->toMouse.back().y = a_y;
				} else {
					if (vs->toMouse.size() >= kMaxQueuedMessages) {
						vs->toMouse.pop_front();
					}
					vs->toMouse.push_back(Impl::MouseInput{ .kind = Impl::MouseInput::Kind::kMove, .x = a_x, .y = a_y });
				}
			}
		}
		_impl->wake.notify_all();
	}

	void UltralightWebRenderer::InjectMouseButton(int a_x, int a_y, int a_button, bool a_down)
	{
		{
			std::scoped_lock lk(_impl->mutex);
			if (auto* vs = _impl->ActiveView()) {
				if (vs->toMouse.size() >= kMaxQueuedMessages) {
					vs->toMouse.pop_front();
				}
				vs->toMouse.push_back(Impl::MouseInput{
					.kind = a_down ? Impl::MouseInput::Kind::kDown : Impl::MouseInput::Kind::kUp,
					.x = a_x,
					.y = a_y,
					.button = a_button });
			}
		}
		_impl->wake.notify_all();
	}

	void UltralightWebRenderer::InjectMouseWheel(int a_x, int a_y, int a_wheelDelta)
	{
		{
			std::scoped_lock lk(_impl->mutex);
			if (auto* vs = _impl->ActiveView()) {
				// Not coalesced (unlike kMove): every notch counts. Bound the
				// queue so a stalled worker can't accumulate scrolls unbounded.
				if (vs->toMouse.size() >= kMaxQueuedMessages) {
					vs->toMouse.pop_front();
				}
				vs->toMouse.push_back(Impl::MouseInput{
					.kind = Impl::MouseInput::Kind::kScroll,
					.x = a_x,
					.y = a_y,
					.wheelDelta = a_wheelDelta });
			}
		}
		_impl->wake.notify_all();
	}
}

#endif  // OSFUI_WITH_ULTRALIGHT
