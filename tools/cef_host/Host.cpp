#include <Windows.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_parser.h"
#include "include/cef_sandbox_win.h"
#include "include/cef_v8.h"
#include "include/cef_version.h"
#include "Core/Ids.h"
#include "Graphics.h"
#include "FrameSchedule.h"
#include "RevealState.h"
#include "FramePacer.h"
#include "FrameTimer.h"
#include "LocalAssets.h"
#include "PendingMessages.h"
#include "AssetStream.h"
#include "Scripts.h"
#include "BrowserLocalUri.h"
#include "BrowserPipe.h"
#include "Win32Util.h"
#include "LanguageTag.h"

namespace osfui::cefhost
{
    namespace msg = browser::msg;
    namespace Json = OSFUI::Json;
    using json = nlohmann::json;
    using win32::ToWide;
    using win32::ToUtf8;

    std::string_view ViewName(std::string_view id) { return id.substr(id.find('/') + 1); }

    class Task final : public CefTask
    {
    public:
        explicit Task(std::function<void()> work) : work_(std::move(work)) {}
        void Execute() override { work_(); }
    private:
        std::function<void()> work_;
        IMPLEMENT_REFCOUNTING(Task);
    };

    // These two bindings exist only in the renderer process. The browser rechecks origin.
    class Binding final : public CefV8Handler
    {
    public:
        bool Execute(const CefString& name, CefRefPtr<CefV8Value>, const CefV8ValueList& arguments,
            CefRefPtr<CefV8Value>&, CefString& exception) override
        {
            if (arguments.size() != 1 || !arguments[0]->IsString()) return false;
            const auto text = arguments[0]->GetStringValue();
            if (text.length() > browser::kMaxPageMessageBytes || text.ToString().size() > browser::kMaxPageMessageBytes) {
                exception = "OSF UI page message exceeds the 64 KiB limit";
                return true;
            }
            auto context = CefV8Context::GetCurrentContext();
            if (!context || !context->GetFrame()->IsMain()) return false;
            auto message = CefProcessMessage::Create(name == "__osfCefPost" ? "osfui.post" : "osfui.reveal");
            message->GetArgumentList()->SetString(0, text);
            context->GetFrame()->SendProcessMessage(PID_BROWSER, message);
            return true;
        }
    private:
        IMPLEMENT_REFCOUNTING(Binding);
    };

    class Host;
    class Application final : public CefApp, public CefBrowserProcessHandler, public CefRenderProcessHandler
    {
    public:
        Host* host{}; // browser process only; outlives CefShutdown
        std::string adapterLuid; // immutable after CefInitialize starts
        CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
        CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }
        void OnContextInitialized() override;
        void OnBeforeCommandLineProcessing(const CefString&, CefRefPtr<CefCommandLine> command) override
        {
            command->AppendSwitchWithValue("proxy-server", "http://osfui-egress.invalid:9");
            command->AppendSwitchWithValue("proxy-bypass-list", "<-loopback>");
            command->AppendSwitchWithValue("host-resolver-rules", "MAP * ~NOTFOUND");
            command->AppendSwitchWithValue("force-webrtc-ip-handling-policy", "disable_non_proxied_udp");
            command->AppendSwitch("disable-quic");
            command->AppendSwitch("disable-background-networking");
            command->AppendSwitch("disable-component-update");
            command->AppendSwitch("disable-renderer-backgrounding");
            command->AppendSwitchWithValue("use-angle", "d3d11");
            if (!adapterLuid.empty()) command->AppendSwitchWithValue("use-adapter-luid", adapterLuid);
        }
        void OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> command) override
        {
            if (!adapterLuid.empty()) command->AppendSwitchWithValue("use-adapter-luid", adapterLuid);
        }
        void OnBrowserCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDictionaryValue> extra) override
        {
            if (extra) documents_[browser->GetIdentifier()] = { extra->GetString("view").ToString(), extra->GetBool("legacy") };
        }
        void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override { documents_.erase(browser->GetIdentifier()); }
        void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override
        {
            // Disable non-request network transports in every frame.
            CefRefPtr<CefV8Value> networkResult;
            CefRefPtr<CefV8Exception> networkError;
            context->Eval(std::string(kNetworkScript), frame->GetURL(), 0, networkResult, networkError);
            const auto found = documents_.find(browser->GetIdentifier());
            if (!frame->IsMain() || found == documents_.end()) return;
            const auto& [id, legacy] = found->second;
            if (!browser::IsTrustedViewDocumentUri(frame->GetURL().ToWString(), browser::kViewHost,
                ToWide(OSFUI::Ids::ModOf(id)), ToWide(ViewName(id)))) return;
            auto handler = CefRefPtr<Binding>(new Binding);
            const auto attributes = static_cast<cef_v8_propertyattribute_t>(V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTDELETE);
            for (const auto* name : { "__osfCefPost", "__osfCefReveal" })
                context->GetGlobal()->SetValue(name, CefV8Value::CreateFunction(name, handler), attributes);
            CefRefPtr<CefV8Value> result;
            CefRefPtr<CefV8Exception> error;
            const auto script = std::string(kBridgeScript) + (legacy ? std::string(kLegacyScript) : "");
            if (!context->Eval(script, frame->GetURL(), 0, result, error)) {
                auto failure = CefProcessMessage::Create("osfui.bridge-error");
                failure->GetArgumentList()->SetString(0, error ? error->GetMessage() : "CEF bridge injection failed");
                frame->SendProcessMessage(PID_BROWSER, failure);
            }
        }
        bool OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
            CefProcessId source, CefRefPtr<CefProcessMessage> message) override
        {
            if (source != PID_BROWSER || !frame->IsMain() || message->GetName() != "osfui.receive") return false;
            auto context = frame->GetV8Context();
            if (!context || !context->Enter()) return true;
            auto receiver = context->GetGlobal()->GetValue("__osfCefReceive");
            if (receiver && receiver->IsFunction()) receiver->ExecuteFunction(nullptr,
                { CefV8Value::CreateString(message->GetArgumentList()->GetString(0)) });
            context->Exit();
            return true;
        }
    private:
        std::map<int, std::pair<std::string, bool>> documents_;
        IMPLEMENT_REFCOUNTING(Application);
    };

    class View final : public CefClient, public CefLifeSpanHandler, public CefRenderHandler,
        public CefLoadHandler, public CefRequestHandler, public CefResourceRequestHandler,
        public CefDisplayHandler, public CefContextMenuHandler, public CefJSDialogHandler,
        public CefDialogHandler, public CefDownloadHandler
    {
    public:
        View(Host& owner, const msg::Navigate& nav, std::filesystem::path root);
        Host& owner;
        const std::string id;
        const std::filesystem::path root;
        const bool legacy;
        const UINT logicalHeight;
        CefRefPtr<CefBrowser> browser;
        Surface surface, popup;
        CefRect popupRect;
        bool hidden{ true }, loaded{}, closing{}, popupShown{};
        RevealState reveal;
        std::uint64_t paintSerial{}, publishedPaintSerial{};
        FramePacer::Clock::time_point lastPaint{};
        int order{};
        CefMouseEvent mouse;
        UINT mouseButtons{};
        std::set<UINT> keys;
        PendingMessages pendingWeb;
        bool pendingOverflowWarned{}, pageSizeWarned{}, pageRateWarned{};
        ULONGLONG pageWindow{};
        UINT pageMessages{};
        int clickCount{}, clickButton{ -1 }, clickX{}, clickY{};
        ULONGLONG clickAt{};
        double Scale() const;
        bool Trusted(const CefString& url) const;
        void ReleaseInput();
        void Reveal();
        void Deliver(std::string payload);
        CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
        CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
        CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
        CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
        CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
        CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
        CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override { return this; }
        CefRefPtr<CefDialogHandler> GetDialogHandler() override { return this; }
        CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }
        void OnAfterCreated(CefRefPtr<CefBrowser> value) override { browser = value; }
        void OnBeforeClose(CefRefPtr<CefBrowser>) override;
        bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int, const CefString&, const CefString&,
            cef_window_open_disposition_t, bool, const CefPopupFeatures&, CefWindowInfo&, CefRefPtr<CefClient>&,
            CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*) override { return true; }
        void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override;
        bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& info) override;
        bool GetScreenPoint(CefRefPtr<CefBrowser>, int x, int y, int& screenX, int& screenY) override;
        void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&, const void*, int, int) override;
        void OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList&, const CefAcceleratedPaintInfo& info) override;
        void OnPopupShow(CefRefPtr<CefBrowser>, bool show) override;
        void OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect) override;
        void OnLoadStart(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, TransitionType) override;
        void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int status) override;
        void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, ErrorCode code, const CefString& text, const CefString& url) override;
        bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, bool, bool) override
        { return !Trusted(request->GetURL()) && !(!frame->IsMain() && browser::IsAllowedBlankFrameUri(request->GetURL().ToWString())); }
        CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
            CefRefPtr<CefRequest> request, bool, bool, const CefString&, bool& disableDefault) override
        { disableDefault = !browser::IsInMemoryViewResourceUri(request->GetURL().ToWString(), browser::kViewHost); return this; }
        ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request,
            CefRefPtr<CefCallback>) override
        { return browser::IsAllowedViewResourceUri(request->GetURL().ToWString(), browser::kViewHost) ? RV_CONTINUE : RV_CANCEL; }
        CefRefPtr<CefResourceHandler> GetResourceHandler(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request) override;
        void OnRenderProcessTerminated(CefRefPtr<CefBrowser>, TerminationStatus status, int code, const CefString& text) override;
        bool OnRenderProcessUnresponsive(CefRefPtr<CefBrowser>, CefRefPtr<CefUnresponsiveProcessCallback> callback) override
        { callback->Terminate(); return true; }
        bool OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefProcessId source,
            CefRefPtr<CefProcessMessage> message) override;
        bool OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle, cef_cursor_type_t type, const CefCursorInfo&) override;
        bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level, const CefString& message, const CefString&, int) override;
        void OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefContextMenuParams>, CefRefPtr<CefMenuModel> model) override
        { model->Clear(); }
        bool OnJSDialog(CefRefPtr<CefBrowser>, const CefString&, JSDialogType, const CefString&, const CefString&,
            CefRefPtr<CefJSDialogCallback>, bool& suppress) override { suppress = true; return false; }
        bool OnBeforeUnloadDialog(CefRefPtr<CefBrowser>, const CefString&, bool, CefRefPtr<CefJSDialogCallback> callback) override
        { callback->Continue(true, ""); return true; }
        bool OnFileDialog(CefRefPtr<CefBrowser>, FileDialogMode, const CefString&, const CefString&,
            const std::vector<CefString>&, const std::vector<CefString>&, const std::vector<CefString>&,
            CefRefPtr<CefFileDialogCallback> callback) override { callback->Cancel(); return true; }
        bool CanDownload(CefRefPtr<CefBrowser>, const CefString&, const CefString&) override { return false; }
    private:
        IMPLEMENT_REFCOUNTING(View);
    };

    class Host
    {
    public:
        browser::Pipe pipe;
        Graphics graphics;
        msg::Init init;
        HANDLE game{};
        std::ofstream log;
        std::map<std::string, CefRefPtr<View>> views;
        std::vector<CefRefPtr<View>> retiring;
        std::string inputTarget;
        UINT width{ 1 }, height{ 1 }, viewportWidth{ 1 }, viewportHeight{ 1 };
        HWND gameWindow{};
        std::uint64_t epoch{}, revealSerial{};
        bool initialized{}, stopping{}, focus{}, windowActive{}, pointerEnabled{};
        std::atomic_bool disconnected{};
        std::atomic_bool queueOverflow{};
        std::thread reader;
        std::mutex queueMutex;
        std::deque<std::string> queue;
        std::size_t queueBytes{};
        std::atomic_bool messageTaskQueued{};
        std::shared_ptr<std::atomic_bool> tasksAlive = std::make_shared<std::atomic_bool>(true);
        FrameSchedule output;
        FrameTimer frameTimer;
        ULONGLONG closeDeadline{};
        int exitCode{};

        ~Host() { tasksAlive->store(false); frameTimer.Stop(); pipe.Close(); if (reader.joinable()) reader.join(); if (game) CloseHandle(game); }
        void Post(std::function<void()> work, std::int64_t delay = 0)
        {
            auto alive = tasksAlive;
            if (!alive->load()) return;
            auto task = CefRefPtr<Task>(new Task([this, alive, work = std::move(work)] {
                if (!alive->load()) return;
                try { work(); }
                catch (const GraphicsError& error) { GraphicsFailure(error); }
                catch (const std::exception& error) { Fatal("cef-host", E_FAIL, error.what()); }
            }));
            const bool posted = delay > 0 ? CefPostDelayedTask(TID_UI, task, delay) : CefPostTask(TID_UI, task);
            // No event can recover a helper whose UI task runner rejects work.
            if (!posted && alive->load()) TerminateProcess(GetCurrentProcess(), 13);
        }
        void QueueMessages()
        {
            if (!messageTaskQueued.exchange(true)) Post([this] { DrainMessages(); });
        }
        bool ScheduleFrame(bool coalesce = false)
        {
            const auto delay = output.Queue(FramePacer::Clock::now(), coalesce);
            if (!delay) return false;
            if (*delay > FramePacer::Clock::duration::zero()) {
                if (!frameTimer.Arm(*delay)) Fatal("cef-timer", HRESULT_FROM_WIN32(GetLastError()), "Cannot arm CEF frame timer");
            }
            else Post([this] { FrameReady(); });
            return true;
        }
        void FrameReady()
        {
            if (!output.Woke()) return;
            if (output.Remaining(FramePacer::Clock::now()) > FramePacer::Clock::duration::zero()) ScheduleFrame();
            else Compose();
        }
        void MarkDirty() { output.Invalidate(); ScheduleFrame(); }
        bool Send(const json& value)
        {
            const bool ok = pipe.WriteMessage(Json::Dump(value)) == browser::Pipe::WriteResult::Written;
            if (!ok) disconnected.store(true);
            return ok;
        }
        void Log(std::string text)
        {
            log << text << '\n'; log.flush();
            Send(msg::ToJson(msg::Log{ .text = std::move(text) }));
        }
        void Fatal(std::string stage, HRESULT code, std::string text)
        {
            if (stopping) return;
            log << stage << ": " << text << " (" << std::hex << static_cast<UINT>(code) << ")\n"; log.flush();
            Send(msg::ToJson(msg::Fatal{ .stage = std::move(stage), .description = std::move(text), .code = static_cast<UINT>(code) }));
            exitCode = 10;
            Stop();
        }
        void GraphicsFailure(const GraphicsError& error)
        {
            Fatal("cef-graphics", error.code, error.what());
            // Do not return a borrowed CEF texture to its pool with an unproven in-flight read.
            if (error.borrowedCopyPending) TerminateProcess(GetCurrentProcess(), 11);
        }
        View* Input()
        {
            const auto found = views.find(inputTarget);
            return found != views.end() && !found->second->hidden && found->second->loaded && !found->second->closing
                ? found->second.get() : nullptr;
        }
        void ReconcileFocus()
        {
            auto* target = Input();
            for (auto& [id, view] : views) {
                if (!view->browser) continue;
                const bool active = view.get() == target && focus && windowActive;
                if (!active) view->ReleaseInput();
                view->browser->GetHost()->SetFocus(active);
            }
        }
        void Stop()
        {
            if (stopping) return;
            stopping = true;
            output.Stop();
            closeDeadline = GetTickCount64() + 5000;
            for (auto& [id, view] : views) {
                view->ReleaseInput(); view->closing = true;
                if (view->browser) {
                    view->browser->GetHost()->CloseDevTools();
                    view->browser->GetHost()->CloseBrowser(true);
                }
            }
            if (!initialized) return;
            Post([this] { CheckShutdown(); });
            Post([this] { CheckShutdown(true); }, 5000);
        }
        void Reader()
        {
            std::string payload;
            while (pipe.ReadMessage(payload)) {
                {
                    std::scoped_lock lock(queueMutex);
                    if (queue.size() >= 512 || queueBytes + payload.size() > 16u * 1024u * 1024u) {
                        queueOverflow.store(true);
                        break;
                    }
                    queueBytes += payload.size(); queue.push_back(std::move(payload));
                }
                QueueMessages();
            }
            disconnected.store(true);
            QueueMessages();
        }
        void Navigate(const msg::Navigate& nav)
        {
            if (!OSFUI::Ids::IsValidQualifiedViewId(nav.id)) { Fatal("cef-navigation", E_INVALIDARG, "Invalid view ID"); return; }
            const auto url = "https://osfui.example/" + nav.id + "/" + nav.entry;
            if (!browser::IsTrustedViewDocumentUri(ToWide(url), browser::kViewHost,
                ToWide(OSFUI::Ids::ModOf(nav.id)), ToWide(ViewName(nav.id)))) {
                Fatal("cef-navigation", E_INVALIDARG, "Invalid view entry"); return;
            }
            if (const auto found = views.find(nav.id); found != views.end()) {
                auto& view = *found->second;
                view.ReleaseInput(); view.loaded = false; view.reveal.Reset();
                view.surface.Clear(); view.popup.Clear(); view.popupShown = false;
                MarkDirty();
                view.browser->GetMainFrame()->LoadURL(url);
                return;
            }
            auto view = CefRefPtr<View>(new View(*this, nav,
                std::filesystem::path(ToWide(nav.legacy ? init.legacyViewsPath : init.viewsPath))));
            CefWindowInfo window;
            window.SetAsWindowless(gameWindow);
            window.shared_texture_enabled = true;
            CefBrowserSettings settings;
            settings.windowless_frame_rate = 60;
            settings.background_color = CefColorSetARGB(0, 0, 0, 0);
            auto extra = CefDictionaryValue::Create();
            extra->SetString("view", nav.id); extra->SetBool("legacy", nav.legacy);
            views.emplace(nav.id, view);
            auto browser = CefBrowserHost::CreateBrowserSync(window, view, url, settings, extra, nullptr);
            if (!browser) { views.erase(nav.id); Fatal("cef-browser", E_FAIL, "CEF browser creation failed"); return; }
            // Hidden documents may load and receive bridge messages without
            // allocating/copying full-size paint surfaces until they are shown.
            browser->GetHost()->WasHidden(true);
            browser->GetHost()->SetFocus(false);
        }
        void InvalidateGeometry()
        {
            MarkDirty();
            for (auto& [id, view] : views) {
                view->ReleaseInput(); view->surface.Clear(); view->popup.Clear(); view->popupShown = false;
                view->browser->GetHost()->NotifyScreenInfoChanged();
                view->browser->GetHost()->WasResized();
                if (!view->hidden) view->Reveal();
            }
        }
        void Mouse(const msg::Mouse& event)
        {
            auto* view = Input();
            if (!view || !focus || !windowActive || !pointerEnabled) return;
            auto browser = view->browser->GetHost();
            view->mouse.x = static_cast<int>(std::floor(event.x / view->Scale()));
            view->mouse.y = static_cast<int>(std::floor(event.y / view->Scale()));
            if (event.kind == "button") {
                if (event.button < 0 || event.button > 2) return;
                const UINT flags[]{ EVENTFLAG_LEFT_MOUSE_BUTTON, EVENTFLAG_RIGHT_MOUSE_BUTTON, EVENTFLAG_MIDDLE_MOUSE_BUTTON };
                if (event.down) view->mouseButtons |= flags[event.button]; else view->mouseButtons &= ~flags[event.button];
            }
            view->mouse.modifiers = view->mouseButtons |
                ((event.modifiers & MK_SHIFT) ? EVENTFLAG_SHIFT_DOWN : 0) |
                ((event.modifiers & MK_CONTROL) ? EVENTFLAG_CONTROL_DOWN : 0);
            if (event.kind == "move") browser->SendMouseMoveEvent(view->mouse, false);
            else if (event.kind == "wheel") browser->SendMouseWheelEvent(view->mouse, 0, event.wheel);
            else if (event.kind == "button") {
                constexpr cef_mouse_button_type_t buttons[]{ MBT_LEFT, MBT_RIGHT, MBT_MIDDLE };
                if (event.down) {
                    const auto now = GetTickCount64();
                    const bool consecutive = view->clickButton == event.button && now - view->clickAt <= GetDoubleClickTime() &&
                        std::abs(event.x - view->clickX) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                        std::abs(event.y - view->clickY) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
                    view->clickCount = consecutive && view->clickCount < 3 ? view->clickCount + 1 : 1;
                    view->clickAt = now; view->clickButton = event.button; view->clickX = event.x; view->clickY = event.y;
                }
                browser->SendMouseClickEvent(view->mouse, buttons[event.button], !event.down, (std::max)(1, view->clickCount));
            }
        }
        void Keyboard(const msg::Keyboard& key)
        {
            auto* view = Input();
            if (!view || !focus || !windowActive) return;
            CefKeyEvent event;
            event.type = key.down ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
            event.windows_key_code = static_cast<int>(key.vk);
            event.native_key_code = static_cast<int>(MapVirtualKeyW(key.vk, MAPVK_VK_TO_VSC) << 16);
            if (key.location == 2 || key.vk == VK_LEFT || key.vk == VK_RIGHT || key.vk == VK_UP || key.vk == VK_DOWN ||
                key.vk == VK_HOME || key.vk == VK_END || key.vk == VK_INSERT || key.vk == VK_DELETE || key.vk == VK_PRIOR || key.vk == VK_NEXT)
                event.native_key_code |= 1 << 24;
            event.is_system_key = key.system;
            event.modifiers = ((key.modifiers & 1) ? EVENTFLAG_ALT_DOWN : 0) |
                ((key.modifiers & 2) ? EVENTFLAG_CONTROL_DOWN : 0) |
                ((key.modifiers & 4) ? EVENTFLAG_COMMAND_DOWN : 0) |
                ((key.modifiers & 8) ? EVENTFLAG_SHIFT_DOWN : 0) |
                (key.keypad ? EVENTFLAG_IS_KEY_PAD : 0) |
                (key.location == 1 ? EVENTFLAG_IS_LEFT : key.location == 2 ? EVENTFLAG_IS_RIGHT : 0) |
                (key.repeat ? EVENTFLAG_IS_REPEAT : 0);
            if (key.down) view->keys.insert(key.vk); else view->keys.erase(key.vk);
            view->browser->GetHost()->SendKeyEvent(event);
        }
        void Text(const msg::TextInput& text)
        {
            auto* view = Input();
            if (!view || !focus || !windowActive) return;
            auto browser = view->browser->GetHost();
            const auto wide = ToWide(text.text);
            if (text.kind == "composition") {
                CefCompositionUnderline underline;
                underline.range = CefRange(0, static_cast<UINT>(wide.size()));
                const auto cursor = (std::min)(text.cursor, static_cast<UINT>(wide.size()));
                browser->ImeSetComposition(wide, { underline }, CefRange(UINT_MAX, UINT_MAX), CefRange(cursor, cursor));
            } else if (text.kind == "commit") browser->ImeCommitText(wide, CefRange(UINT_MAX, UINT_MAX), 0);
            else if (text.kind == "cancel") browser->ImeCancelComposition();
            else if (text.kind == "char") for (wchar_t ch : wide) {
                CefKeyEvent event; event.type = KEYEVENT_CHAR; event.windows_key_code = ch;
                event.character = event.unmodified_character = ch;
                browser->SendKeyEvent(event);
            }
        }
        void Handle(const json& raw)
        {
            const auto type = Json::Get(raw, "type", "");
            if (type == msg::Init::kType) return; // consumed before CefInitialize
            if (type == msg::Navigate::kType) { Navigate(msg::FromJson<msg::Navigate>(raw)); return; }
            if (type == msg::Resize::kType) {
                const auto value = msg::FromJson<msg::Resize>(raw);
                width = (std::max)(1u, value.width); height = (std::max)(1u, value.height);
                output.Unblock(); MarkDirty(); return;
            }
            if (type == msg::Viewport::kType) {
                const auto value = msg::FromJson<msg::Viewport>(raw);
                epoch = value.presentationEpoch;
                viewportWidth = (std::max)(1u, value.width); viewportHeight = (std::max)(1u, value.height);
                InvalidateGeometry(); return;
            }
            if (type == msg::FrameAck::kType) {
                const auto value = msg::FromJson<msg::FrameAck>(raw);
                graphics.Acknowledge(value.slot, value.serial);
                output.Unblock(); ScheduleFrame(); return;
            }
            if (type == msg::GameWindow::kType) {
                gameWindow = reinterpret_cast<HWND>(msg::FromJson<msg::GameWindow>(raw).hwnd);
                for (auto& [id, view] : views) view->browser->GetHost()->NotifyScreenInfoChanged();
                return;
            }
            if (type == msg::PointerInput::kType) {
                pointerEnabled = msg::FromJson<msg::PointerInput>(raw).enabled;
                if (!pointerEnabled) for (auto& [id, view] : views) view->ReleaseInput();
                return;
            }
            if (type == msg::Focus::kType) { focus = msg::FromJson<msg::Focus>(raw).focused; ReconcileFocus(); return; }
            if (type == msg::WindowActive::kType) { windowActive = msg::FromJson<msg::WindowActive>(raw).active; ReconcileFocus(); return; }
            if (type == msg::Mouse::kType) { Mouse(msg::FromJson<msg::Mouse>(raw)); return; }
            if (type == msg::Keyboard::kType) { Keyboard(msg::FromJson<msg::Keyboard>(raw)); return; }
            if (type == msg::TextInput::kType) { Text(msg::FromJson<msg::TextInput>(raw)); return; }
            const auto found = views.find(Json::Get(raw, "view", ""));
            if (found == views.end()) return;
            auto view = found->second; // close callbacks may erase the map entry
            if (type == msg::SetHidden::kType) {
                const auto value = msg::FromJson<msg::SetHidden>(raw);
                epoch = value.presentationEpoch;
                const bool wasHidden = view->hidden;
                view->hidden = value.hidden;
                if (view->hidden) {
                    view->ReleaseInput(); view->browser->GetHost()->WasHidden(true);
                    view->surface.Clear(); view->popup.Clear(); view->popupShown = false;
                    view->lastPaint = {}; view->reveal.Reset();
                }
                else if (wasHidden) { view->browser->GetHost()->WasHidden(false); view->Reveal(); }
                MarkDirty(); ReconcileFocus();
            } else if (type == msg::SetOrder::kType) { view->order = msg::FromJson<msg::SetOrder>(raw).order; MarkDirty(); }
            else if (type == msg::SetInputTarget::kType) { inputTarget = view->id; ReconcileFocus(); }
            else if (type == msg::PostWeb::kType) view->Deliver(msg::FromJson<msg::PostWeb>(raw).json);
            else if (type == msg::Back::kType) {
                const auto value = msg::FromJson<msg::Back>(raw);
                if (view->popupShown) {
                    CefKeyEvent escape; escape.windows_key_code = VK_ESCAPE;
                    escape.type = KEYEVENT_RAWKEYDOWN; view->browser->GetHost()->SendKeyEvent(escape);
                    escape.type = KEYEVENT_KEYUP; view->browser->GetHost()->SendKeyEvent(escape);
                } else Send(msg::ToJson(msg::BackUnhandled{ .view = view->id, .presentationEpoch = value.presentationEpoch }));
            } else if (type == msg::DestroyView::kType) {
                view->ReleaseInput(); view->closing = true; view->hidden = true;
                retiring.push_back(view); views.erase(found);
                view->browser->GetHost()->CloseDevTools();
                view->browser->GetHost()->CloseBrowser(true); MarkDirty();
            } else if (type == msg::OpenDevTools::kType && init.devMode) {
                // Explicit developer action; never invoked by page-requested window.open.
                CefWindowInfo window; window.SetAsPopup(gameWindow, "OSF UI CEF DevTools");
                view->browser->GetHost()->ShowDevTools(window, nullptr, CefBrowserSettings{}, CefPoint{});
            }
        }
        void Compose()
        {
            if (!output.Pending()) return;
            std::vector<View*> visible;
            bool complete = true, anyVisible = false;
            for (auto& [id, view] : views) if (!view->hidden && !view->closing) {
                anyVisible = true;
                if (!view->loaded || !view->reveal.Ready() || !view->surface.texture) {
                    complete = false;
                    continue;
                }
                visible.push_back(view.get());
            }
            if (!anyVisible) { output.NoViews(); return; }
            const auto now = FramePacer::Clock::now();
            const bool fresh = std::any_of(visible.begin(), visible.end(), [](auto* view) {
                return view->paintSerial != view->publishedPaintSerial;
            });
            const bool waiting = visible.size() > 1 && std::any_of(visible.begin(), visible.end(), [now](auto* view) {
                return view->paintSerial == view->publishedPaintSerial &&
                    now - view->lastPaint < 2 * FramePacer::period;
            });
            if (fresh && waiting && ScheduleFrame(true)) return;
            std::stable_sort(visible.begin(), visible.end(), [](auto* a, auto* b) { return a->order < b->order; });
            const float x = (static_cast<float>(width) - viewportWidth) * 0.5f;
            const float y = (static_cast<float>(height) - viewportHeight) * 0.5f;
            std::vector<Layer> layers;
            for (auto* view : visible) {
                layers.push_back({ &view->surface, x, y, static_cast<float>(viewportWidth), static_cast<float>(viewportHeight) });
                if (view->popupShown && view->popup.texture) {
                    const float scale = static_cast<float>(view->Scale());
                    layers.push_back({ &view->popup, x + view->popupRect.x * scale, y + view->popupRect.y * scale,
                        view->popupRect.width * scale, view->popupRect.height * scale });
                }
            }
            if (graphics.Publish(layers, width, height, epoch, complete)) {
                for (auto* view : visible) view->publishedPaintSerial = view->paintSerial;
                output.Published(FramePacer::Clock::now());
            } else output.Blocked(); // New paints replace surfaces; an ack wakes publication.
        }
        void DrainMessages()
        {
            if (queueOverflow.exchange(false))
                Fatal("host-connection", E_OUTOFMEMORY, "CEF incoming pipe queue exceeded 512 messages or 16 MiB; restarting helper");
            if (disconnected.load()) Stop();
            for (int count = 0; !stopping && count < 128; ++count) {
                std::string payload;
                { std::scoped_lock lock(queueMutex);
                  if (queue.empty()) break;
                  payload = std::move(queue.front()); queue.pop_front(); queueBytes -= payload.size(); }
                if (auto value = Json::Parse(payload)) Handle(*value);
            }
            // Clear before checking the tail: arrivals on either side of this
            // boundary must schedule a task, including batches larger than 128.
            messageTaskQueued.store(false);
            bool more;
            { std::scoped_lock lock(queueMutex); more = !queue.empty(); }
            if (!stopping && (more || disconnected.load())) QueueMessages();
            ScheduleFrame();
        }
        void CheckShutdown(bool deadlineTask = false)
        {
            std::erase_if(retiring, [](const auto& view) { return !view->browser; });
            if (stopping) {
                const bool open = std::any_of(views.begin(), views.end(), [](const auto& item) { return item.second->browser != nullptr; }) ||
                    std::any_of(retiring.begin(), retiring.end(), [](const auto& view) { return view->browser != nullptr; });
                if (!open) { tasksAlive->store(false); CefQuitMessageLoop(); return; }
                const auto now = GetTickCount64();
                if (now >= closeDeadline) {
                    log << "CEF shutdown timed out; terminating this helper without premature CefShutdown\n"; log.flush();
                    TerminateProcess(GetCurrentProcess(), 12);
                }
                if (deadlineTask) Post([this] { CheckShutdown(true); }, closeDeadline - now + 1);
            }
        }
        void Maintenance()
        {
            if (disconnected.load() || WaitForSingleObject(game, 0) != WAIT_TIMEOUT) Stop();
            if (!stopping) {
                Send(msg::ToJson(msg::Heartbeat{}));
                Post([this] { Maintenance(); }, browser::kHeartbeatIntervalMs);
            }
        }
    };

    View::View(Host& host, const msg::Navigate& nav, std::filesystem::path assets) :
        owner(host), id(nav.id), root(std::move(assets)), legacy(nav.legacy), logicalHeight((std::max)(1u, nav.logicalHeight)) {}
    double View::Scale() const { return static_cast<double>(owner.viewportHeight) / logicalHeight; }
    bool View::Trusted(const CefString& url) const
    { return browser::IsTrustedViewDocumentUri(url.ToWString(), browser::kViewHost, ToWide(OSFUI::Ids::ModOf(id)), ToWide(ViewName(id))); }
    void View::OnBeforeClose(CefRefPtr<CefBrowser>)
    {
        browser = nullptr; surface.Clear(); popup.Clear();
        owner.Post([host = &owner] { host->CheckShutdown(); });
    }
    void View::GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect)
    { rect = CefRect(0, 0, (std::max)(1, static_cast<int>(std::lround(owner.viewportWidth / Scale()))), static_cast<int>(logicalHeight)); }
    bool View::GetScreenInfo(CefRefPtr<CefBrowser> b, CefScreenInfo& info)
    {
        info.device_scale_factor = static_cast<float>(Scale());
        CefRect rect;
        GetViewRect(b, rect); info.rect = info.available_rect = rect;
        info.depth = 32; info.depth_per_component = 8;
        return true;
    }
    bool View::GetScreenPoint(CefRefPtr<CefBrowser>, int x, int y, int& screenX, int& screenY)
    {
        POINT position{ static_cast<LONG>((static_cast<double>(owner.width) - owner.viewportWidth) / 2 + x * Scale()),
            static_cast<LONG>((static_cast<double>(owner.height) - owner.viewportHeight) / 2 + y * Scale()) };
        if (owner.gameWindow) ClientToScreen(owner.gameWindow, &position);
        screenX = position.x; screenY = position.y; return true;
    }
    void View::OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&, const void*, int, int)
    { owner.Fatal("cef-paint", E_NOTIMPL, "CEF supplied software pixels; this prototype requires accelerated OSR"); }
    void View::OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList& dirtyRects, const CefAcceleratedPaintInfo& info)
    {
        if (closing || hidden || owner.stopping) return;
        try {
            const auto& rect = info.extra.visible_rect;
            const PaintRect visible{ rect.x, rect.y, rect.width, rect.height };
            const auto counter = info.extra.has_capture_counter ? std::optional<std::uint64_t>(info.extra.capture_counter) : std::nullopt;
            auto& target = type == PET_POPUP ? popup : surface;
            const auto copy = target.damage.Select(visible, dirtyRects, counter);
            owner.graphics.CopyPaint(info.shared_texture_handle, visible, copy, target);
            target.damage.Copied(visible, counter);
            if (type == PET_VIEW) { ++paintSerial; lastPaint = FramePacer::Clock::now(); }
            if (type == PET_VIEW) reveal.Painted();
            if (!hidden && (type == PET_VIEW || popupShown)) owner.MarkDirty();
        } catch (const GraphicsError& error) { owner.GraphicsFailure(error); }
    }
    void View::OnPopupShow(CefRefPtr<CefBrowser>, bool show)
    { popupShown = show; if (!show) popup.Clear(); if (!hidden) owner.MarkDirty(); }
    void View::OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect)
    { popupRect = rect; if (!hidden && popupShown) owner.MarkDirty(); }
    void View::ReleaseInput()
    {
        if (!browser) return;
        auto host = browser->GetHost();
        constexpr cef_mouse_button_type_t buttons[]{ MBT_LEFT, MBT_RIGHT, MBT_MIDDLE };
        constexpr UINT flags[]{ EVENTFLAG_LEFT_MOUSE_BUTTON, EVENTFLAG_RIGHT_MOUSE_BUTTON, EVENTFLAG_MIDDLE_MOUSE_BUTTON };
        for (int i = 0; i < 3; ++i) if (mouseButtons & flags[i]) {
            mouseButtons &= ~flags[i]; mouse.modifiers = mouseButtons;
            host->SendMouseClickEvent(mouse, buttons[i], true, 1);
        }
        for (const auto key : keys) {
            CefKeyEvent event; event.type = KEYEVENT_KEYUP; event.windows_key_code = static_cast<int>(key);
            host->SendKeyEvent(event);
        }
        keys.clear(); clickCount = 0; clickButton = -1;
        host->ImeCancelComposition(); host->SendCaptureLostEvent();
    }
    void View::Reveal()
    {
        reveal.Begin(++owner.revealSerial);
        if (!loaded || hidden || !browser) return;
        browser->GetMainFrame()->ExecuteJavaScript(
            "requestAnimationFrame(()=>requestAnimationFrame(()=>window.__osfCefReveal?.('" + std::to_string(reveal.Token()) + "')))",
            browser->GetMainFrame()->GetURL(), 0);
    }
    void View::Deliver(std::string payload)
    {
        if (!loaded || !browser) {
            if (!pendingWeb.Push(std::move(payload)) && !std::exchange(pendingOverflowWarned, true))
                owner.Log("view '" + id + "': pending web messages exceeded the 64-message/8-MiB cap; dropping oldest");
            return;
        }
        auto message = CefProcessMessage::Create("osfui.receive");
        message->GetArgumentList()->SetString(0, payload);
        browser->GetMainFrame()->SendProcessMessage(PID_RENDERER, message);
    }
    void View::OnLoadStart(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, TransitionType)
    { if (frame->IsMain()) { ReleaseInput(); loaded = false; reveal.Reset(); popupShown = false; owner.MarkDirty(); } }
    void View::OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int status)
    {
        if (!frame->IsMain() || closing) return;
        loaded = status >= 200 && status < 400 && Trusted(frame->GetURL());
        owner.Send(msg::ToJson(msg::LoadEvent{ .view = id, .failed = !loaded, .url = frame->GetURL().ToString(),
            .description = loaded ? "" : "CEF document load failed", .code = status }));
        if (!loaded) return;
        auto messages = pendingWeb.Take(); pendingOverflowWarned = false;
        for (auto& message : messages) Deliver(std::move(message));
        if (!hidden) Reveal();
        owner.ReconcileFocus();
    }
    void View::OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, ErrorCode code, const CefString& text, const CefString& url)
    {
        if (!frame->IsMain() || code == ERR_ABORTED || closing) return;
        ReleaseInput(); loaded = false; reveal.Reset();
        owner.MarkDirty();
        owner.Send(msg::ToJson(msg::LoadEvent{ .view = id, .failed = true, .url = url.ToString(), .description = text.ToString(), .code = code }));
    }
    class LocalResource final : public CefResourceHandler
    {
    public:
        LocalResource(std::filesystem::path file, CefString mime, CefResponse::HeaderMap headers) :
            file_(std::move(file)), mime_(std::move(mime)), headers_(std::move(headers)) {}
        bool Open(CefRefPtr<CefRequest> request, bool& handle, CefRefPtr<CefCallback>) override
        {
            handle = true;
            // Without a matching validator, If-Range requires the full representation.
            return stream_.Open(file_, request->GetMethod() == "HEAD",
                request->GetHeaderByName("If-Range").empty() ? request->GetHeaderByName("Range").ToString() : "");
        }
        void GetResponseHeaders(CefRefPtr<CefResponse> response, int64_t& length, CefString&) override
        {
            const auto& range = stream_.response;
            response->SetStatus(range.status);
            response->SetStatusText(range.status == 206 ? "Partial Content" : range.status == 416 ? "Range Not Satisfiable" : "OK");
            response->SetMimeType(mime_);
            headers_.emplace("Accept-Ranges", "bytes");
            headers_.emplace("Content-Length", std::to_string(range.length));
            if (range.status == 206) headers_.emplace("Content-Range", "bytes " + std::to_string(range.offset) + "-" +
                std::to_string(range.offset + range.length - 1) + "/" + std::to_string(stream_.size));
            else if (range.status == 416) headers_.emplace("Content-Range", "bytes */" + std::to_string(stream_.size));
            response->SetHeaderMap(headers_);
            length = stream_.Remaining();
        }
        bool Read(void* data, int capacity, int& read, CefRefPtr<CefResourceReadCallback>) override
        { read = stream_.Read(data, capacity); return read > 0; }
        bool Skip(int64_t count, int64_t& skipped, CefRefPtr<CefResourceSkipCallback>) override
        { skipped = stream_.Skip(count); return skipped >= 0; }
        void Cancel() override {}
    private:
        const std::filesystem::path file_;
        const CefString mime_;
        CefResponse::HeaderMap headers_;
        AssetStream stream_;
        IMPLEMENT_REFCOUNTING(LocalResource);
    };

    CefRefPtr<CefResourceHandler> View::GetResourceHandler(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request)
    {
        if (request->GetMethod() != "GET" && request->GetMethod() != "HEAD") return nullptr;
        const auto uri = browser::ParseHttpsUri(request->GetURL().ToWString());
        if (!uri || uri->host != browser::kViewHost) return nullptr;
        const auto decoded = CefURIDecode(uri->path, false, static_cast<cef_uri_unescape_rule_t>(UU_NORMAL | UU_SPACES | UU_URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS)).ToWString();
        const auto file = ResolveAsset(root, decoded);
        if (!file) return nullptr; // default network handling is disabled
        auto extension = file->extension().wstring();
        if (!extension.empty()) extension.erase(0, 1);
        auto mime = CefGetMimeType(extension);
        if (mime.empty()) mime = "application/octet-stream";
        CefResponse::HeaderMap headers;
        headers.emplace("Cache-Control", "no-store");
        headers.emplace("X-Content-Type-Options", "nosniff");
        headers.emplace("Content-Security-Policy", "connect-src https://osfui.example data: blob:; worker-src 'none'; object-src 'none'");
        return new LocalResource(*file, mime, std::move(headers));
    }
    void View::OnRenderProcessTerminated(CefRefPtr<CefBrowser>, TerminationStatus status, int code, const CefString& text)
    {
        ReleaseInput(); loaded = false; reveal.Reset(); surface.Clear(); popup.Clear(); owner.MarkDirty();
        owner.Send(msg::ToJson(msg::LoadEvent{ .view = id, .failed = true,
            .description = "CEF renderer terminated (status " + std::to_string(status) + "): " + text.ToString(), .code = code }));
    }
    bool View::OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefProcessId source,
        CefRefPtr<CefProcessMessage> message)
    {
        if (source != PID_RENDERER || !frame->IsMain() || !Trusted(frame->GetURL()) || closing) return false;
        const auto payload = message->GetArgumentList()->GetString(0).ToString();
        if (payload.size() > browser::kMaxPageMessageBytes) {
            if (!std::exchange(pageSizeWarned, true)) owner.Log("view '" + id + "': dropped page message over the 64 KiB limit");
            return true;
        }
        if (message->GetName() == "osfui.post") {
            const auto now = GetTickCount64();
            if (now - pageWindow >= 1000) { pageWindow = now; pageMessages = 0; }
            if (++pageMessages > browser::kMaxPageMessagesPerSecond) {
                if (!std::exchange(pageRateWarned, true)) owner.Log("view '" + id + "': page-message rate exceeded 128/s; excess messages are dropped");
                return true;
            }
            owner.Send(msg::ToJson(msg::WebMessage{ .view = id, .json = payload }));
        } else if (message->GetName() == "osfui.reveal") {
            if (!hidden && loaded && payload == std::to_string(reveal.Token()) && reveal.Sentinel()) {
                browser->GetHost()->Invalidate(PET_VIEW);
                // A refresh immediately after the show paint can produce no
                // further callback. A static page has no later paint to open the
                // reveal gate. Retry once after two 60 Hz periods, without
                // accepting pre-reveal pixels or polling an idle document.
                const auto token = reveal.Token();
                owner.Post([view = CefRefPtr<View>(this), token] {
                    if (view->browser && !view->closing && !view->hidden && view->loaded &&
                        view->reveal.NeedsRefresh(token))
                        view->browser->GetHost()->Invalidate(PET_VIEW);
                }, 34);
            }
        } else if (message->GetName() == "osfui.bridge-error") owner.Fatal("cef-bridge", E_FAIL, payload);
        else return false;
        return true;
    }
    bool View::OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle, cef_cursor_type_t type, const CefCursorInfo&)
    {
        if (owner.inputTarget != id) return true;
        UINT cursor = 32512; // IDC_ARROW; transport accepts system IDs, not process-local handles
        if (type == CT_IBEAM) cursor = 32513;
        else if (type == CT_HAND) cursor = 32649;
        else if (type == CT_CROSS) cursor = 32515;
        else if (type == CT_WAIT) cursor = 32514;
        else if (type == CT_NONE) cursor = 0;
        else if (type == CT_NOTALLOWED) cursor = 32648;
        else if (type == CT_EASTWESTRESIZE) cursor = 32644;
        else if (type == CT_NORTHSOUTHRESIZE) cursor = 32645;
        owner.Send(msg::ToJson(msg::Cursor{ .id = cursor })); return true;
    }
    bool View::OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level, const CefString& message, const CefString&, int)
    {
        if (owner.init.devMode) owner.Send(msg::ToJson(msg::Console{ .view = id,
            .json = Json::Dump(json{ { "type", level >= LOGSEVERITY_ERROR ? "error" : level >= LOGSEVERITY_WARNING ? "warning" : "log" },
                { "args", json::array({ json{ { "type", "string" }, { "value", message.ToString() } } }) } }) }));
        return true;
    }
    void Application::OnContextInitialized() { if (host) host->Post([value = host] { value->Maintenance(); }); }

    int Run(HINSTANCE instance, void* sandbox)
    {
        CefMainArgs args(instance);
        CefRefPtr<Application> app(new Application);
        const int child = CefExecuteProcess(args, app, sandbox);
        if (child >= 0) return child;
        auto command = CefCommandLine::CreateCommandLine();
        command->InitFromString(GetCommandLineW());
        const auto pipeName = command->GetSwitchValue("pipe").ToWString();
        const auto pidText = command->GetSwitchValue("game-pid").ToWString();
        const auto logPath = command->GetSwitchValue("log").ToWString();
        const auto pid = static_cast<DWORD>(wcstoul(pidText.c_str(), nullptr, 10));
        if (pipeName.empty() || !pid || !sandbox) return 1;
        Host host;
        host.log.open(std::filesystem::path(logPath), std::ios::trunc);
        host.pipe.PrepareForOpen();
        if (!host.pipe.Connect(pipeName, 15000) || host.pipe.ServerProcessId() != pid) return 2;
        host.game = OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!host.game) { host.Fatal("cef-init", HRESULT_FROM_WIN32(GetLastError()), "Cannot open game process"); return 3; }
        host.Send(msg::ToJson(msg::Hello{ .protocolVersion = browser::kBrowserHostProtocolVersion, .runtimeVersion = "CEF " CEF_VERSION, .backend = "cef", .capabilities = browser::kRequiredHostCapabilities }));
        std::string initial;
        if (!host.pipe.ReadMessage(initial, 10000)) return 4;
        const auto parsed = Json::Parse(initial);
        if (!parsed || Json::Get(*parsed, "type", "") != msg::Init::kType) return 5;
        host.init = msg::FromJson<msg::Init>(*parsed);
        host.width = host.viewportWidth = host.init.width;
        host.height = host.viewportHeight = host.init.height;
        host.gameWindow = reinterpret_cast<HWND>(host.init.topLevelHwnd);
        const std::filesystem::path profile(ToWide(host.init.userDataDir));
        if (!profile.is_absolute() || !host.width || !host.height) return 6;
        std::error_code ec;
        std::filesystem::create_directories(profile, ec);
        if (ec) { host.Fatal("cef-profile", HRESULT_FROM_WIN32(ec.value()), "Cannot create CEF profile"); return 7; }
        try {
            host.graphics.Initialize({ host.init.adapterLuidLow, static_cast<LONG>(host.init.adapterLuidHigh) }, host.game,
                [&host](const json& message) { return host.Send(message); });
        } catch (const GraphicsError& error) { host.GraphicsFailure(error); return 8; }
        CefSettings settings;
        settings.windowless_rendering_enabled = true;
        settings.background_color = CefColorSetARGB(0, 0, 0, 0);
        CefString(&settings.root_cache_path) = profile.wstring();
        CefString(&settings.cache_path) = (profile / "Default").wstring();
        CefString(&settings.log_file) = logPath + L".chromium.log";
        const auto language = browser::LanguageTag::FromGameLanguage(host.init.language);
        if (!language.empty()) CefString(&settings.locale) = language;
        app->host = &host;
        // ANGLE expects signed high-part, unsigned low-part, in decimal.
        app->adapterLuid = std::to_string(static_cast<std::int32_t>(host.init.adapterLuidHigh)) + "," +
            std::to_string(host.init.adapterLuidLow);
        if (!CefInitialize(args, settings, app, sandbox)) return CefGetExitCode();
        host.initialized = true;
        try { host.frameTimer.Start([&host] { host.Post([&host] { host.FrameReady(); }); }); }
        catch (const std::exception& error) { host.Fatal("cef-timer", E_FAIL, error.what()); }
        host.Log("CEF " CEF_VERSION "; accelerated off-screen rendering; sandbox enabled; shared host protocol " +
            std::to_string(browser::kBrowserHostProtocolVersion) + "; event-driven messages; coalesced 60 Hz output");
        host.reader = std::thread([&host] { host.Reader(); });
        CefRunMessageLoop();
        host.tasksAlive->store(false);
        host.frameTimer.Stop();
        host.pipe.Close();
        if (host.reader.joinable()) host.reader.join();
        host.views.clear(); host.retiring.clear();
        CefShutdown();
        app->host = nullptr;
        return host.exitCode;
    }
}

CEF_BOOTSTRAP_EXPORT int RunWinMain(HINSTANCE instance, LPTSTR, int, void* sandbox, cef_version_info_t*)
{
    return osfui::cefhost::Run(instance, sandbox);
}
