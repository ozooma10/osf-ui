#include "render/UltralightWebRenderer.h"

#if defined(SWUI_WITH_ULTRALIGHT)

	#include "core/Log.h"
	#include "platform/WindowsPlatform.h"

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

namespace SWUI
{
	namespace
	{
		namespace ul = ultralight;

		constexpr std::size_t kMaxQueuedMessages = 64;
		constexpr std::size_t kMaxLoggedTextLen = 512;
		constexpr auto        kWorkerFrameInterval = std::chrono::milliseconds(16);  // ~60 Hz

		[[nodiscard]] std::string ToUTF8(const ul::String& a_str)
		{
			const auto& s8 = a_str.utf8();
			return std::string(s8.data(), s8.length());
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
			void SetViewRoot(std::filesystem::path a_dir) { _viewRoot = std::move(a_dir); }

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
				// "resources/..." (Config::resource_path_prefix) is served from
				// the support dir; everything else from the active view.
				const bool isResource = rel.begin() != rel.end() && *rel.begin() == "resources";
				const auto& base = isResource ? _resourceBase : _viewRoot;
				if (base.empty()) {
					return std::nullopt;
				}
				return base / rel;
			}

			std::filesystem::path _resourceBase;  // contains resources/ (ICU data)
			std::filesystem::path _viewRoot;      // active view directory
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
			std::uint32_t vk{ 0 };
			bool          down{ false };
		};

		struct MouseInput
		{
			enum class Kind { kMove, kDown, kUp };
			Kind kind{ Kind::kMove };
			int  x{ 0 };
			int  y{ 0 };
			int  button{ 0 };  // MouseButton order: 0=left, 1=right, 2=middle
		};

		struct Frame
		{
			std::vector<std::uint8_t> pixels;
			std::uint32_t             width{ 0 };
			std::uint32_t             height{ 0 };
			std::uint32_t             strideBytes{ 0 };
			std::uint64_t             index{ 0 };
		};

		// ---- per-view state (one per loaded view, keyed by manifest id) ----
		// Today exactly one view is hosted (the configured view = activeViewId)
		// and the IWebRenderer interface stays single-view, targeting the active
		// view. The keyed map + per-view grouping is the structural seam for
		// hosting several at once (renderer-plan.md M2.1) without reshaping the
		// threading model.
		struct ViewState
		{
			std::string id;

			// shared with the game thread (guarded by Impl::mutex)
			std::optional<ViewManifest>                            pendingLoad;
			std::optional<std::pair<std::uint32_t, std::uint32_t>> pendingResize;
			std::deque<std::string>                                toWeb;     // game -> page
			std::deque<std::string>                                toNative;  // page -> game
			std::deque<KeyInput>                                   toInput;   // game -> view (keyboard)
			std::deque<MouseInput>                                 toMouse;   // game -> view (mouse)

			// frame exchange: pendingFrame/pendingFresh guarded by Impl::frameMutex;
			// frontFrame is game-thread-only and backs the FrameBufferView we hand out.
			Frame pendingFrame;
			bool  pendingFresh{ false };
			Frame frontFrame;

			// worker-thread-only
			ul::RefPtr<ul::View>    view;
			Frame                   backFrame;
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

		// The JSC postMessage callback is a plain function pointer with no
		// user-data slot, so it finds us through this. One renderer per process.
		// (Attributing a message to its SOURCE view, when several are hosted,
		// will key on the JSContextRef; today it routes to the active view.)
		static inline std::atomic<Impl*> sActive{ nullptr };

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
			};

			std::unique_lock lock(mutex);
			while (!stopRequested) {
				wake.wait_for(lock, kWorkerFrameInterval);
				if (stopRequested) {
					break;
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
					work.push_back(std::move(w));
				}
				lock.unlock();

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

				std::vector<ViewState*> live;
				live.reserve(work.size());
				for (auto& w : work) {
					live.push_back(w.vs);
				}
				PumpUltralight(live);

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
			platform.set_file_system(fileSystem);
			// AppCore is linked ONLY for this DirectWrite-backed font loader;
			// no window/app machinery is used.
			REX::INFO("UltralightWebRenderer: worker acquiring platform font loader");
			platform.set_font_loader(ul::GetPlatformFontLoader());

			REX::INFO("UltralightWebRenderer: worker calling Renderer::Create()");
			renderer = ul::Renderer::Create();
			if (!renderer) {
				REX::ERROR("UltralightWebRenderer: Renderer::Create() failed (check Ultralight log lines above)");
				return false;
			}
			// In-memory session: no cookies/local storage ever touch disk.
			session = renderer->CreateSession(false, "starfieldwebui");
			REX::INFO("UltralightWebRenderer: Ultralight renderer created (CPU rendering, in-memory session)");
			return true;
		}

		void CreateAndLoadView(ViewState& a_vs, const ViewManifest& a_manifest)
		{
			fileSystem->SetViewRoot(a_manifest.rootDir);

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

			const auto url = "file:///" + a_manifest.entry;
			REX::INFO("UltralightWebRenderer: loading view '{}' -> {} (root {})",
				a_manifest.id, url, a_manifest.rootDir.string());
			a_vs.view->LoadURL(url.c_str());
		}

		void PumpUltralight(const std::vector<ViewState*>& a_views)
		{
			renderer->Update();
			for (auto* vs : a_views) {
				if (vs->view && vs->domReady) {
					FlushStashedMessages(*vs);
				}
			}
			renderer->RefreshDisplay(0);
			renderer->Render();  // repaints only the views that need it
			for (auto* vs : a_views) {
				if (vs->view) {
					HarvestFrame(*vs);
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

			{
				std::scoped_lock lk(frameMutex);
				std::swap(a_vs.backFrame, a_vs.pendingFrame);
				a_vs.pendingFresh = true;
			}
		}

		// ================= JS bridge (worker thread) =================

		static JSValueRef PostMessageCallback(
			JSContextRef a_ctx, JSObjectRef, JSObjectRef, size_t a_argc,
			const JSValueRef a_args[], JSValueRef*)
		{
			auto* self = sActive.load();
			if (self && a_argc >= 1) {
				auto json = JSValueToUTF8(a_ctx, a_args[0]);
				std::scoped_lock lk(self->mutex);
				// Single-view today: route to the active view. Multi-view will
				// attribute by JSContextRef (the source view's context).
				if (auto* vs = self->ActiveView()) {
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

		void InjectBridge(ViewState& a_vs)
		{
			auto scopedCtx = a_vs.view->LockJSContext();
			JSContextRef ctx = scopedCtx->ctx();
			JSObjectRef  global = JSContextGetGlobalObject(ctx);

			// window.starfield = { postMessage: <native fn> }
			// The object stays extensible so the page can attach onMessage;
			// postMessage itself is read-only.
			JSObjectRef starfield = JSObjectMake(ctx, nullptr, nullptr);
			JSStringRef postName = JSStringCreateWithUTF8CString("postMessage");
			JSObjectRef postFn = JSObjectMakeFunctionWithCallback(ctx, postName, &PostMessageCallback);
			JSObjectSetProperty(ctx, starfield, postName, postFn,
				kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete, nullptr);
			JSStringRelease(postName);

			JSStringRef starfieldName = JSStringCreateWithUTF8CString("starfield");
			JSObjectSetProperty(ctx, global, starfieldName, starfield, kJSPropertyAttributeNone, nullptr);
			JSStringRelease(starfieldName);
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

			JSStringRef starfieldName = JSStringCreateWithUTF8CString("starfield");
			JSValueRef  starfieldVal = JSObjectGetProperty(ctx, global, starfieldName, nullptr);
			JSStringRelease(starfieldName);
			if (!JSValueIsObject(ctx, starfieldVal)) {
				REX::WARN("UltralightWebRenderer: window.starfield missing; dropped native->web message");
				return;
			}
			JSObjectRef starfield = JSValueToObject(ctx, starfieldVal, nullptr);

			JSStringRef onName = JSStringCreateWithUTF8CString("onMessage");
			JSValueRef  onVal = JSObjectGetProperty(ctx, starfield, onName, nullptr);
			JSStringRelease(onName);
			if (!JSValueIsObject(ctx, onVal) || !JSObjectIsFunction(ctx, JSValueToObject(ctx, onVal, nullptr))) {
				REX::WARN("UltralightWebRenderer: window.starfield.onMessage is not a function; dropped native->web message");
				return;
			}

			JSStringRef jsonStr = JSStringCreateWithUTF8CString(a_json.c_str());
			JSValueRef  arg = JSValueMakeString(ctx, jsonStr);
			JSStringRelease(jsonStr);
			JSValueRef exception = nullptr;
			JSObjectCallAsFunction(ctx, JSValueToObject(ctx, onVal, nullptr), starfield, 1, &arg, &exception);
			if (exception) {
				REX::WARN("UltralightWebRenderer: onMessage threw: {}",
					JSValueToUTF8(ctx, exception).substr(0, kMaxLoggedTextLen));
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
			ul::GetKeyIdentifierFromVirtualKeyCode(key.virtual_key_code, key.key_identifier);

			if (a_key.down) {
				key.type = ul::KeyEvent::kType_RawKeyDown;
				a_vs.view->FireKeyEvent(key);

				// Emit a Char event for text-producing keys (skip while Ctrl/Alt
				// are held so shortcuts like Ctrl+C don't type a character).
				if (!a_vs.modCtrl && !a_vs.modAlt) {
					ul::String keyText;
					ul::GetKeyFromVirtualKeyCode(key.virtual_key_code, a_vs.modShift, keyText);
					const auto utf8 = ToUTF8(keyText);
					// Single printable character -> real text input.
					if (utf8.size() == 1 && static_cast<unsigned char>(utf8[0]) >= 0x20) {
						ul::KeyEvent ch;
						ch.type = ul::KeyEvent::kType_Char;
						ch.modifiers = key.modifiers;
						ch.text = keyText;
						ch.unmodified_text = keyText;
						a_vs.view->FireKeyEvent(ch);
					}
				}
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
			if (vs) {
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
			// Give the view input focus so routed key events reach the focused
			// DOM element (the page autofocuses its input).
			vs->view->Focus();
			FlushStashedMessages(*vs);
		}

		void OnFailLoading(ul::View*, std::uint64_t, bool a_isMainFrame, const ul::String& a_url,
			const ul::String& a_description, const ul::String& a_errorDomain, int a_errorCode) override
		{
			REX::ERROR("UltralightWebRenderer: load failed (mainFrame={}, url={}): {} [{} {}]",
				a_isMainFrame, ToUTF8(a_url), ToUTF8(a_description), ToUTF8(a_errorDomain), a_errorCode);
		}

		void OnAddConsoleMessage(ul::View*, const ul::ConsoleMessage& a_message) override
		{
			const auto text = ToUTF8(a_message.message()).substr(0, kMaxLoggedTextLen);
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
			// Create-or-replace the view entry for this id and make it active.
			// The game thread owns the map slot; the worker creates the actual
			// ul::View when it picks up pendingLoad.
			auto& slot = _impl->views[a_manifest.id];
			if (!slot) {
				slot = std::make_unique<Impl::ViewState>();
			}
			slot->id = a_manifest.id;
			slot->pendingLoad = a_manifest;
			_impl->activeViewId = a_manifest.id;
		}
		_impl->wake.notify_all();
	}

	void UltralightWebRenderer::Resize(std::uint32_t a_width, std::uint32_t a_height)
	{
		{
			std::scoped_lock lk(_impl->mutex);
			if (auto* vs = _impl->ActiveView()) {
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

		// The worker self-paces; our job here is only to hand page->game
		// messages to the runtime on the game thread. Drain every view's queue
		// (one view today); the message handler is currently view-agnostic.
		std::deque<std::string> inbound;
		{
			std::scoped_lock lk(_impl->mutex);
			for (auto& [id, vs] : _impl->views) {
				while (!vs->toNative.empty()) {
					inbound.push_back(std::move(vs->toNative.front()));
					vs->toNative.pop_front();
				}
			}
		}
		if (_impl->onWebMessage) {
			for (const auto& json : inbound) {
				_impl->onWebMessage(json);
			}
		}
	}

	std::optional<FrameBufferView> UltralightWebRenderer::Render()
	{
		// Resolve the active view under `mutex`, then touch its frame under
		// `frameMutex` (never both held at once). The ViewState pointer stays
		// valid for the renderer's lifetime; frontFrame is game-thread-only.
		Impl::ViewState* vs = nullptr;
		{
			std::scoped_lock lk(_impl->mutex);
			vs = _impl->ActiveView();
		}
		if (!vs) {
			return std::nullopt;
		}
		{
			std::scoped_lock lk(_impl->frameMutex);
			if (vs->pendingFresh) {
				std::swap(vs->pendingFrame, vs->frontFrame);
				vs->pendingFresh = false;
			}
		}
		const auto& front = vs->frontFrame;
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

	void UltralightWebRenderer::SendMessageToWeb(std::string_view a_json)
	{
		{
			std::scoped_lock lk(_impl->mutex);
			auto* vs = _impl->ActiveView();
			if (!vs) {
				return;
			}
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
				vs->toInput.push_back(Impl::KeyInput{ .vk = a_vkCode, .down = a_down });
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
}

#endif  // SWUI_WITH_ULTRALIGHT
