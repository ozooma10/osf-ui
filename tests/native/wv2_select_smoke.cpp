// Real WebView2 + WGC proof; never grants the browser native focus or launches Starfield.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <objbase.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <DispatcherQueue.h>
#include <d3d11.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <wrl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include "EmbeddedScripts.h"
#include "FormControls.h"
#include "Wv2CdpInput.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
using namespace osfui::wv2;
namespace fs = std::filesystem;
using json = nlohmann::json;

static void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

template<class Predicate> static void PumpUntil(Predicate done)
{
    const auto deadline = GetTickCount64() + 15000;
    while (!done()) {
        Require(GetTickCount64() < deadline, "WebView2/WGC operation timed out");
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

static std::wstring Wide(std::string_view text)
{
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

int wmain(int argc, wchar_t** argv)
{
    try {
        Require(argc == 2, "usage: wv2-select-smoke <Starcade launcher source directory>");
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        const auto originalForeground = GetForegroundWindow();
        const auto output = fs::absolute(fs::path(".tmp") / ("select-smoke-" + std::to_string(GetCurrentProcessId())));
        const auto site = output / "site";
        fs::create_directories(site / "games");
        const fs::path source(argv[1]);
        fs::copy(source / "games/video-poker", site / "games/video-poker", fs::copy_options::recursive);
        for (const auto* name : { "game.css", "casino.css", "casino-bank.js", "host.js" }) {
            fs::copy_file(source / "games" / name, site / "games" / name);
        }
        std::ofstream(site / "index.html") << R"html(<!doctype html><meta charset="utf-8">
            <style>body{margin:0;background:#123}iframe{width:780px;height:470px;border:0}
            #plain{margin:10px;width:150px}#list{display:none}</style>
            <select id="plain"><option value="a">Alpha</option><option value="b">Beta</option></select>
            <select id="list" multiple size="3"><option>A</option><option>B</option></select>
            <iframe id="game" sandbox="allow-scripts allow-same-origin" src="games/video-poker/index.html"></iframe>
            <div id="stamp" style="position:fixed;right:0;bottom:0;width:4px;height:4px;z-index:99999"></div>
            <script>window.escapes=0;window.exits=0;window.changes=0;
            addEventListener('keydown',e=>{if(e.key==='Escape')++escapes});
            addEventListener('message',e=>{if(e.data?.type==='exit')++exits});
            plain.addEventListener('change',()=>++changes);</script>)html";

        const auto parent = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"OSF UI select smoke",
            WS_POPUP, -32000, -32000, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(parent != nullptr, "offscreen owner creation failed");
        ShowWindow(parent, SW_SHOWNOACTIVATE);
        winrt::Windows::System::DispatcherQueueController dispatcher{ nullptr };
        DispatcherQueueOptions dq{ sizeof(dq), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA };
        winrt::check_hresult(CreateDispatcherQueueController(dq,
            reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(dispatcher))));
        winrt::Windows::UI::Composition::Compositor compositor;
        auto root = compositor.CreateContainerVisual();
        root.Size({ 800, 600 });
        ComPtr<ICoreWebView2Environment> environment;
        auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        options->put_AdditionalBrowserArguments(L"--disable-backgrounding-occluded-windows --disable-renderer-backgrounding --disable-features=CalculateNativeWinOcclusion,ApplyNativeOcclusionToCompositor");
        bool done = false;
        winrt::check_hresult(CreateCoreWebView2EnvironmentWithOptions(nullptr, (output / "profile").c_str(), options.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([&](HRESULT hr, ICoreWebView2Environment* value) {
                if (SUCCEEDED(hr)) environment = value;
                done = true; return S_OK;
            }).Get()));
        PumpUntil([&] { return done; });
        Require(environment != nullptr, "environment creation failed");
        ComPtr<ICoreWebView2Environment3> environment3;
        winrt::check_hresult(environment.As(&environment3));
        ComPtr<ICoreWebView2CompositionController> composition;
        done = false;
        winrt::check_hresult(environment3->CreateCoreWebView2CompositionController(parent,
            Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>([&](HRESULT hr, ICoreWebView2CompositionController* value) {
                if (SUCCEEDED(hr)) composition = value;
                done = true; return S_OK;
            }).Get()));
        PumpUntil([&] { return done; });
        Require(composition != nullptr, "controller creation failed");
        ComPtr<ICoreWebView2Controller> controller;
        winrt::check_hresult(composition.As(&controller));
        controller->put_Bounds(RECT{ 0, 0, 800, 600 });
        controller->put_IsVisible(TRUE);
        winrt::check_hresult(composition->put_RootVisualTarget(root.as<::IUnknown>().get()));
        ComPtr<ICoreWebView2> web;
        winrt::check_hresult(controller->get_CoreWebView2(&web));
        ComPtr<ICoreWebView2_3> web3;
        winrt::check_hresult(web.As(&web3));
        winrt::check_hresult(web3->SetVirtualHostNameToFolderMapping(L"select.osfui.test", site.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS));
        done = false;
        // Production injection runs in both the parent and the real sandboxed game frame.
        winrt::check_hresult(web->AddScriptToExecuteOnDocumentCreated(GetEmbeddedScript(EmbeddedScript::FormControls).c_str(),
            Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>([&](HRESULT hr, LPCWSTR) {
                done = SUCCEEDED(hr); return S_OK;
            }).Get()));
        PumpUntil([&] { return done; });
        EventRegistrationToken navigation{};
        done = false;
        web->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>([&](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) {
            done = true; return S_OK;
        }).Get(), &navigation);
        web->Navigate(L"https://select.osfui.test/index.html");
        PumpUntil([&] { return done; });
        web->remove_NavigationCompleted(navigation);
        auto script = [&](const wchar_t* expression) {
            json result;
            bool finished = false;
            winrt::check_hresult(web->ExecuteScript(expression, Callback<ICoreWebView2ExecuteScriptCompletedHandler>([&](HRESULT hr, LPCWSTR value) {
                if (SUCCEEDED(hr) && value) result = json::parse(winrt::to_string(value));
                finished = true; return S_OK;
            }).Get()));
            PumpUntil([&] { return finished; });
            return result;
        };
        auto check = [&](const wchar_t* expression) {
            auto result = script(expression);
            if (result != true) std::wcerr << expression << L" -> " << Wide(result.dump()) << L'\n';
            Require(result == true, "DOM assertion failed");
        };
        auto cdp = [&](const wchar_t* method, const json& params) {
            bool finished = false;
            HRESULT status = E_FAIL;
            winrt::check_hresult(web->CallDevToolsProtocolMethod(method, Wide(params.dump()).c_str(),
                Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>([&](HRESULT hr, LPCWSTR) {
                    status = hr; finished = true; return S_OK;
                }).Get()));
            PumpUntil([&] { return finished; });
            winrt::check_hresult(status);
        };
        auto key = [&](std::uint32_t vk, const char* name) {
            msg::Keyboard event{ .vk = vk, .down = true, .key = name, .code = name };
            cdp(L"Input.dispatchKeyEvent", CdpKeyParams(event));
            // Physical Enter also produces WM_CHAR, forwarded by HandleTextInput.
            if (vk == VK_RETURN) cdp(L"Input.dispatchKeyEvent", { { "type", "char" }, { "text", "\r" }, { "unmodifiedText", "\r" } });
            event.down = false;
            cdp(L"Input.dispatchKeyEvent", CdpKeyParams(event));
        };
        auto click = [&](const wchar_t* rectangle) {
            const auto point = script(rectangle);
            const POINT p{ point.at(0).get<LONG>(), point.at(1).get<LONG>() };
            winrt::check_hresult(composition->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, 0, p));
            winrt::check_hresult(composition->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON, 0, p));
            winrt::check_hresult(composition->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, 0, p));
        };
        cdp(L"Emulation.setFocusEmulationEnabled", { { "enabled", true } });
        check(L"CSS.supports('appearance','base-select') && getComputedStyle(plain).appearance==='base-select' && getComputedStyle(list).appearance!=='base-select'");
        PumpUntil([&] { return script(L"!!game.contentDocument?.querySelector('#wager')") == true; });
        check(L"getComputedStyle(game.contentDocument.querySelector('#wager')).appearance==='base-select'");
        // An unmistakable test-only marker. Only an expanded picker paints it.
        check(L"(()=>{for(const d of [document,game.contentDocument]){const s=new d.defaultView.CSSStyleSheet();s.replaceSync('select::picker(select){background:rgb(240,0,220)!important;color:white!important} option{background:transparent!important}');d.adoptedStyleSheets.push(s)}return true})()");

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
        ComPtr<IDXGIDevice> dxgi;
        winrt::check_hresult(device.As(&dxgi));
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
        using namespace winrt::Windows::Graphics;
        auto captureDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        auto item = Capture::GraphicsCaptureItem::CreateFromVisual(root);
        auto pool = Capture::Direct3D11CaptureFramePool::Create(captureDevice, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, { 800, 600 });
        auto session = pool.CreateCaptureSession(item);
        session.IsCursorCaptureEnabled(false);
        session.StartCapture();
        ComPtr<ID3D11Texture2D> staging;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 800; desc.Height = 600; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, &staging));
        unsigned phase = 0;
        auto pixels = [&](bool open, const char* name) {
            // A new visible stamp is painted after the DOM assertion. An older
            // queued WGC frame cannot satisfy this step, even if a prior popup
            // happened to paint the same marker colour.
            ++phase;
            script((L"stamp.style.backgroundColor='rgb(17," + std::to_wstring(phase) + L",33)'").c_str());
            std::size_t marked = 0;
            PumpUntil([&] {
                auto frame = pool.TryGetNextFrame();
                if (!frame) return false;
                auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                ComPtr<ID3D11Texture2D> texture;
                winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
                context->CopyResource(staging.Get(), texture.Get());
                D3D11_MAPPED_SUBRESOURCE data{};
                winrt::check_hresult(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &data));
                marked = 0;
                for (unsigned y = 0; y < 600; ++y) {
                    const auto* row = static_cast<const unsigned char*>(data.pData) + y * data.RowPitch;
                    for (unsigned x = 0; x < 800; ++x) {
                        const auto* p = row + x * 4;
                        if (p[0] > 190 && p[1] < 25 && p[2] > 210 && p[3] > 240) ++marked;
                    }
                }
                const auto* stamp = static_cast<const unsigned char*>(data.pData) + 599 * data.RowPitch + 799 * 4;
                const bool fresh = stamp[0] == 33 && stamp[1] == phase && stamp[2] == 17;
                const bool matched = fresh && (open ? marked > 1000 : marked == 0);
                if (matched) {
                    std::ofstream file(output / (std::string(name) + ".bmp"), std::ios::binary);
                    BITMAPFILEHEADER header{ 0x4d42, 54 + 800 * 600 * 4, 0, 0, 54 };
                    BITMAPINFOHEADER info{ sizeof(info), 800, -600, 1, 32, BI_RGB };
                    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
                    file.write(reinterpret_cast<const char*>(&info), sizeof(info));
                    for (unsigned y = 0; y < 600; ++y) file.write(static_cast<const char*>(data.pData) + y * data.RowPitch, 800 * 4);
                }
                context->Unmap(staging.Get(), 0);
                return matched;
            });
            std::cout << name << ": " << marked << " picker pixels\n";
        };
        pixels(false, "closed");
        click(L"(()=>{const r=plain.getBoundingClientRect();return [Math.round(r.x+r.width/2),Math.round(r.y+r.height/2)]})()");
        PumpUntil([&] { return script(L"plain.matches(':open')") == true; });
        check(kHasOpenSelectScript);
        pixels(true, "parent-open");
        key(VK_ESCAPE, "Escape");
        check(L"!plain.matches(':open') && escapes===0 && plain.value==='a'");
        Require(script(kHasOpenSelectScript) == false, "dismissed select still owns Back");
        pixels(false, "parent-dismissed");
        key(VK_RETURN, "Enter");
        check(L"plain.matches(':open')");
        key(VK_DOWN, "ArrowDown");
        key(VK_RETURN, "Enter");
        check(L"plain.value==='b' && changes===1 && !plain.matches(':open')");

        click(L"(()=>{const r=game.contentDocument.querySelector('#wager').getBoundingClientRect(),f=game.getBoundingClientRect();return [Math.round(f.x+r.x+r.width/2),Math.round(f.y+r.y+r.height/2)]})()");
        PumpUntil([&] { return script(L"game.contentDocument.querySelector('#wager').matches(':open')") == true; });
        check(kHasOpenSelectScript);
        check(L"!plain.matches(':open')");
        pixels(true, "starcade-open");
        key(VK_ESCAPE, "Escape");
        check(L"!game.contentDocument.querySelector('#wager').matches(':open') && exits===0");
        pixels(false, "starcade-dismissed");
        key(VK_RETURN, "Enter");
        check(L"game.contentDocument.querySelector('#wager').matches(':open')");
        click(L"(()=>{const r=game.contentDocument.querySelector('#wager option:last-child').getBoundingClientRect(),f=game.getBoundingClientRect();return [Math.round(f.x+r.x+r.width/2),Math.round(f.y+r.y+r.height/2)]})()");
        PumpUntil([&] { return script(L"game.contentDocument.querySelector('#wager').value==='500'") == true; });
        check(L"!game.contentDocument.querySelector('#wager').matches(':open') && exits===0");
        pixels(false, "starcade-selected");
        // Also retain a human-readable capture with the authored page styling,
        // after removing only the diagnostic stylesheet installed above.
        check(L"(()=>{for(const d of [document,game.contentDocument])d.adoptedStyleSheets.pop();return true})()");
        click(L"(()=>{const r=game.contentDocument.querySelector('#wager').getBoundingClientRect(),f=game.getBoundingClientRect();return [Math.round(f.x+r.x+r.width/2),Math.round(f.y+r.y+r.height/2)]})()");
        PumpUntil([&] { return script(L"game.contentDocument.querySelector('#wager').matches(':open')") == true; });
        pixels(false, "starcade-default-style");
        key(VK_ESCAPE, "Escape");
        check(L"!game.contentDocument.querySelector('#wager').matches(':open') && exits===0");
        key(VK_ESCAPE, "Escape");
        PumpUntil([&] { return script(L"exits===1") == true; });
        DWORD foregroundPid{};
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
        std::cout << "foreground before=" << originalForeground << " after=" << GetForegroundWindow()
            << " foreground pid=" << foregroundPid << " test pid=" << GetCurrentProcessId()
            << " local focus=" << GetFocus() << '\n';
        Require(GetForegroundWindow() == originalForeground && foregroundPid != GetCurrentProcessId(), "foreground focus changed");
        session.Close(); pool.Close(); controller->Close(); DestroyWindow(parent);
        std::cout << "PASS: fresh captures of parent and Starcade iframe pickers; mouse/keyboard selection; Escape dismisses before game exit; foreground unchanged.\n";
        std::wcout << L"Artifacts: " << output.c_str() << L'\n';
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::wcerr << L"Select smoke FAILED: " << error.message().c_str() << L'\n';
    } catch (const std::exception& error) {
        std::cerr << "Select smoke FAILED: " << error.what() << '\n';
    }
    return 1;
}
