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
		// ---- game-thread state ----
		RendererConfig    config;
		WebMessageHandler onWebMessage;

		// ---- shared state (guarded by mutex) ----
		std::mutex                                              mutex;
		std::condition_variable                                 wake;
		bool                                                    stopRequested{ false };
		std::optional<ViewManifest>                             pendingLoad;
		std::optional<std::pair<std::uint32_t, std::uint32_t>>  pendingResize;
		std::deque<std::string>                                 toWeb;     // game -> page
		std::deque<std::string>                                 toNative;  // page -> game

		struct KeyInput
		{
			std::uint32_t vk{ 0 };
			bool          down{ false };
		};
		std::deque<KeyInput> toInput;  // game -> view (keyboard)

		struct MouseInput
		{
			enum class Kind { kMove, kDown, kUp };
			Kind kind{ Kind::kMove };
			int  x{ 0 };
			int  y{ 0 };
			int  button{ 0 };  // MouseButton order: 0=left, 1=right, 2=middle
		};
		std::deque<MouseInput> toMouse;  // game -> view (mouse)

		// ---- frame exchange ----
		struct Frame
		{
			std::vector<std::uint8_t> pixels;
			std::uint32_t             width{ 0 };
			std::uint32_t             height{ 0 };
			std::uint32_t             strideBytes{ 0 };
			std::uint64_t             index{ 0 };
		};
		std::mutex frameMutex;
		Frame      pendingFrame;
		bool       pendingFresh{ false };
		Frame      frontFrame;  // game thread only; backs the FrameBufferView we hand out

		// ---- worker-thread-only state ----
		std::thread             worker;
		ul::RefPtr<ul::Renderer> renderer;
		ul::RefPtr<ul::Session>  session;
		ul::RefPtr<ul::View>     view;
		SandboxFileSystem*       fileSystem{ nullptr };  // owned by Platform for process lifetime
		Frame                    backFrame;
		std::uint64_t            paintCount{ 0 };
		bool                     domReady{ false };
		std::deque<std::string>  stashedToWeb;  // arrived before the page was ready
		bool                     dumpedFirstFrame{ false };

		// worker-thread keyboard modifier state (tracked from the VK stream)
		bool modShift{ false };
		bool modCtrl{ false };
		bool modAlt{ false };
		bool loggedFirstKey{ false };

		// The JSC postMessage callback is a plain function pointer with no
		// user-data slot, so it finds us through this. One renderer per
		// process (Ultralight allows only one anyway).
		static inline std::atomic<Impl*> sActive{ nullptr };

		// ================= worker thread =================

		void WorkerMain()
		{
			if (!SetupPlatform()) {
				REX::ERROR("UltralightWebRenderer: worker setup failed; the overlay will never produce frames");
				return;
			}

			std::unique_lock lock(mutex);
			while (!stopRequested) {
				wake.wait_for(lock, kWorkerFrameInterval);
				if (stopRequested) {
					break;
				}

				// Move pending work local, then run Ultralight unlocked so the
				// game thread is never blocked behind WebCore.
				auto load = std::exchange(pendingLoad, std::nullopt);
				auto resize = std::exchange(pendingResize, std::nullopt);
				std::deque<std::string> outbound;
				outbound.swap(toWeb);
				std::deque<KeyInput> keys;
				keys.swap(toInput);
				std::deque<MouseInput> mice;
				mice.swap(toMouse);
				lock.unlock();

				if (load) {
					CreateAndLoadView(*load);
				}
				if (resize && view) {
					view->Resize(resize->first, resize->second);
				}
				for (auto& msg : outbound) {
					stashedToWeb.push_back(std::move(msg));
				}
				for (const auto& key : keys) {
					ProcessKeyInput(key);
				}
				for (const auto& m : mice) {
					ProcessMouseInput(m);
				}
				PumpUltralight();

				lock.lock();
			}
			lock.unlock();

			// Release on this thread — WebCore objects must die where they lived.
			view = nullptr;
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

		void CreateAndLoadView(const ViewManifest& a_manifest)
		{
			fileSystem->SetViewRoot(a_manifest.rootDir);

			ul::ViewConfig viewConfig;
			viewConfig.is_accelerated = false;  // CPU BitmapSurface (Phase 1)
			viewConfig.is_transparent = a_manifest.transparent;
			viewConfig.initial_device_scale = 1.0;
			view = renderer->CreateView(a_manifest.width, a_manifest.height, viewConfig, session);
			if (!view) {
				REX::ERROR("UltralightWebRenderer: CreateView({}x{}) failed", a_manifest.width, a_manifest.height);
				return;
			}
			view->set_load_listener(this);
			view->set_view_listener(this);
			domReady = false;

			const auto url = "file:///" + a_manifest.entry;
			REX::INFO("UltralightWebRenderer: loading view '{}' -> {} (root {})",
				a_manifest.id, url, a_manifest.rootDir.string());
			view->LoadURL(url.c_str());
		}

		void PumpUltralight()
		{
			renderer->Update();
			if (!view) {
				return;
			}
			if (domReady) {
				FlushStashedMessages();
			}
			renderer->RefreshDisplay(0);
			renderer->Render();
			HarvestFrame();
		}

		void HarvestFrame()
		{
			auto* surface = static_cast<ul::BitmapSurface*>(view->surface());
			if (!surface || surface->dirty_bounds().IsEmpty()) {
				return;
			}

			const auto stride = surface->row_bytes();
			const auto byteCount = surface->size();
			if (auto* pixels = surface->LockPixels()) {
				backFrame.pixels.assign(
					static_cast<const std::uint8_t*>(pixels),
					static_cast<const std::uint8_t*>(pixels) + byteCount);
				backFrame.width = surface->width();
				backFrame.height = surface->height();
				backFrame.strideBytes = stride;
				backFrame.index = ++paintCount;
				surface->UnlockPixels();
			} else {
				return;
			}
			surface->ClearDirtyBounds();

			if (paintCount == 1) {
				REX::INFO("UltralightWebRenderer: first paint ({}x{}, stride {})",
					backFrame.width, backFrame.height, backFrame.strideBytes);
			}
			if (config.devMode && !dumpedFirstFrame && domReady) {
				// Phase 1 exit criterion: prove real pixels by dumping the
				// first frame painted after DOM ready (earlier paints are a
				// blank pre-style flash).
				dumpedFirstFrame = true;
				const auto dumpPath = (config.dataDir / "ultralight" / "first-frame.png").string();
				if (surface->bitmap()->WritePNG(dumpPath.c_str())) {
					REX::INFO("UltralightWebRenderer: post-DOM frame dumped to {}", dumpPath);
				} else {
					REX::WARN("UltralightWebRenderer: failed to write {}", dumpPath);
				}
			}

			{
				std::scoped_lock lk(frameMutex);
				std::swap(backFrame, pendingFrame);
				pendingFresh = true;
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
				if (self->toNative.size() < kMaxQueuedMessages) {
					self->toNative.push_back(std::move(json));
				} else {
					static std::once_flag once;
					Log::WarnOnce(once, "UltralightWebRenderer: web->native queue full; dropping messages");
				}
			}
			return JSValueMakeUndefined(a_ctx);
		}

		void InjectBridge()
		{
			auto scopedCtx = view->LockJSContext();
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

		void FlushStashedMessages()
		{
			while (!stashedToWeb.empty()) {
				const auto json = std::move(stashedToWeb.front());
				stashedToWeb.pop_front();
				DeliverToWeb(json);
			}
		}

		void DeliverToWeb(const std::string& a_json)
		{
			auto scopedCtx = view->LockJSContext();
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

		[[nodiscard]] unsigned CurrentModifiers() const
		{
			unsigned m = 0;
			if (modShift) {
				m |= ul::KeyEvent::kMod_ShiftKey;
			}
			if (modCtrl) {
				m |= ul::KeyEvent::kMod_CtrlKey;
			}
			if (modAlt) {
				m |= ul::KeyEvent::kMod_AltKey;
			}
			return m;
		}

		void ProcessKeyInput(const KeyInput& a_key)
		{
			if (!view) {
				return;
			}

			// Track modifier state from the VK stream (Windows VK_* codes).
			switch (a_key.vk) {
			case 0x10:           // VK_SHIFT
			case 0xA0: case 0xA1:  // VK_LSHIFT / VK_RSHIFT
				modShift = a_key.down;
				break;
			case 0x11:           // VK_CONTROL
			case 0xA2: case 0xA3:  // VK_LCONTROL / VK_RCONTROL
				modCtrl = a_key.down;
				break;
			case 0x12:           // VK_MENU (Alt)
			case 0xA4: case 0xA5:  // VK_LMENU / VK_RMENU
				modAlt = a_key.down;
				break;
			default:
				break;
			}

			if (!loggedFirstKey) {
				loggedFirstKey = true;
				REX::INFO("UltralightWebRenderer: first key routed into the view (vk={:#x}, down={})", a_key.vk, a_key.down);
			}

			ul::KeyEvent key;
			key.virtual_key_code = static_cast<int>(a_key.vk);
			key.native_key_code = static_cast<int>(a_key.vk);
			key.modifiers = CurrentModifiers();
			ul::GetKeyIdentifierFromVirtualKeyCode(key.virtual_key_code, key.key_identifier);

			if (a_key.down) {
				key.type = ul::KeyEvent::kType_RawKeyDown;
				view->FireKeyEvent(key);

				// Emit a Char event for text-producing keys (skip while Ctrl/Alt
				// are held so shortcuts like Ctrl+C don't type a character).
				if (!modCtrl && !modAlt) {
					ul::String keyText;
					ul::GetKeyFromVirtualKeyCode(key.virtual_key_code, modShift, keyText);
					const auto utf8 = ToUTF8(keyText);
					// Single printable character -> real text input.
					if (utf8.size() == 1 && static_cast<unsigned char>(utf8[0]) >= 0x20) {
						ul::KeyEvent ch;
						ch.type = ul::KeyEvent::kType_Char;
						ch.modifiers = key.modifiers;
						ch.text = keyText;
						ch.unmodified_text = keyText;
						view->FireKeyEvent(ch);
					}
				}
			} else {
				key.type = ul::KeyEvent::kType_KeyUp;
				view->FireKeyEvent(key);
			}
		}

		// ================= mouse input (worker thread) =================

		void ProcessMouseInput(const MouseInput& a_m)
		{
			if (!view) {
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
			view->FireMouseEvent(ev);
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

		void OnWindowObjectReady(ul::View*, std::uint64_t, bool a_isMainFrame, const ul::String&) override
		{
			if (a_isMainFrame) {
				InjectBridge();
			}
		}

		void OnDOMReady(ul::View*, std::uint64_t, bool a_isMainFrame, const ul::String& a_url) override
		{
			if (a_isMainFrame) {
				domReady = true;
				REX::INFO("UltralightWebRenderer: DOM ready ({})", ToUTF8(a_url));
				// Give the view input focus so routed key events reach the
				// focused DOM element (the page autofocuses its input).
				view->Focus();
				FlushStashedMessages();
			}
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
			_impl->pendingLoad = a_manifest;
		}
		_impl->wake.notify_all();
	}

	void UltralightWebRenderer::Resize(std::uint32_t a_width, std::uint32_t a_height)
	{
		{
			std::scoped_lock lk(_impl->mutex);
			_impl->pendingResize = { a_width, a_height };
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
		// messages to the runtime on the game thread.
		std::deque<std::string> inbound;
		{
			std::scoped_lock lk(_impl->mutex);
			inbound.swap(_impl->toNative);
		}
		if (_impl->onWebMessage) {
			for (const auto& json : inbound) {
				_impl->onWebMessage(json);
			}
		}
	}

	std::optional<FrameBufferView> UltralightWebRenderer::Render()
	{
		{
			std::scoped_lock lk(_impl->frameMutex);
			if (_impl->pendingFresh) {
				std::swap(_impl->pendingFrame, _impl->frontFrame);
				_impl->pendingFresh = false;
			}
		}
		const auto& front = _impl->frontFrame;
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
			if (_impl->toWeb.size() >= kMaxQueuedMessages) {
				static std::once_flag once;
				Log::WarnOnce(once, "UltralightWebRenderer: native->web queue full; dropping messages");
				return;
			}
			_impl->toWeb.emplace_back(a_json);
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
			// Bound the queue so a stuck worker can't accumulate unbounded
			// keystrokes; dropping the oldest keeps the newest responsive.
			if (_impl->toInput.size() >= kMaxQueuedMessages) {
				_impl->toInput.pop_front();
			}
			_impl->toInput.push_back(Impl::KeyInput{ .vk = a_vkCode, .down = a_down });
		}
		_impl->wake.notify_all();
	}

	void UltralightWebRenderer::InjectMouseMove(int a_x, int a_y)
	{
		{
			std::scoped_lock lk(_impl->mutex);
			// Coalesce: only the latest position matters, so collapse runs of
			// pending moves into one to keep the queue (and worker) light.
			if (!_impl->toMouse.empty() && _impl->toMouse.back().kind == Impl::MouseInput::Kind::kMove) {
				_impl->toMouse.back().x = a_x;
				_impl->toMouse.back().y = a_y;
			} else {
				if (_impl->toMouse.size() >= kMaxQueuedMessages) {
					_impl->toMouse.pop_front();
				}
				_impl->toMouse.push_back(Impl::MouseInput{ .kind = Impl::MouseInput::Kind::kMove, .x = a_x, .y = a_y });
			}
		}
		_impl->wake.notify_all();
	}

	void UltralightWebRenderer::InjectMouseButton(int a_x, int a_y, int a_button, bool a_down)
	{
		{
			std::scoped_lock lk(_impl->mutex);
			if (_impl->toMouse.size() >= kMaxQueuedMessages) {
				_impl->toMouse.pop_front();
			}
			_impl->toMouse.push_back(Impl::MouseInput{
				.kind = a_down ? Impl::MouseInput::Kind::kDown : Impl::MouseInput::Kind::kUp,
				.x = a_x,
				.y = a_y,
				.button = a_button });
		}
		_impl->wake.notify_all();
	}
}

#endif  // SWUI_WITH_ULTRALIGHT
