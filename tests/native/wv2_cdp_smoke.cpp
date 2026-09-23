// Optional real-runtime smoke test. No Starfield, OS input injection, or focus grant.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <objbase.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <wrl.h>
#include "Wv2CdpInput.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
using namespace osfui::wv2;

static void Require(bool ok, const char* message)
{
	if (!ok) throw std::runtime_error(message);
}

template<class Predicate> void PumpUntil(Predicate done)
{
	const auto deadline = ::GetTickCount64() + 20000;
	while (!done()) {
		Require(::GetTickCount64() < deadline, "WebView2 operation timed out");
		MSG message{};
		while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
			::TranslateMessage(&message);
			::DispatchMessageW(&message);
		}
		::MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
	}
}

static std::wstring Wide(std::string_view text)
{
	const auto length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
	std::wstring result(length, L'\0');
	::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length);
	return result;
}

int main()
{
	try {
		Require(SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM initialization failed");
		const auto originalForeground = ::GetForegroundWindow();
		const auto parent = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"OSF UI CDP smoke",
			WS_POPUP, -32000, -32000, 640, 480, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		Require(parent != nullptr, "could not create offscreen owner");
		::ShowWindow(parent, SW_SHOWNOACTIVATE);
		const auto profile = std::filesystem::absolute(std::filesystem::path(".tmp") / ("cdp-smoke-" + std::to_string(::GetCurrentProcessId())));
		std::filesystem::create_directories(profile);
		ComPtr<ICoreWebView2Environment> environment;
		bool done = false;
		auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
		options->put_AdditionalBrowserArguments(L"--disable-backgrounding-occluded-windows --disable-renderer-backgrounding");
		Require(SUCCEEDED(::CreateCoreWebView2EnvironmentWithOptions(nullptr, profile.c_str(), options.Get(),
			Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([&](HRESULT hr, ICoreWebView2Environment* value) {
				if (SUCCEEDED(hr)) environment = value;
				done = true;
				return S_OK;
			}).Get())), "environment creation request failed");
		PumpUntil([&] { return done; });
		Require(environment != nullptr, "environment creation failed");
		ComPtr<ICoreWebView2Environment3> environment3;
		Require(SUCCEEDED(environment.As(&environment3)), "composition API unavailable");
		ComPtr<ICoreWebView2CompositionController> composition;
		done = false;
		Require(SUCCEEDED(environment3->CreateCoreWebView2CompositionController(parent,
			Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>([&](HRESULT hr, ICoreWebView2CompositionController* value) {
				if (SUCCEEDED(hr)) composition = value;
				done = true;
				return S_OK;
			}).Get())), "controller creation request failed");
		PumpUntil([&] { return done; });
		Require(composition != nullptr, "controller creation failed");
		ComPtr<ICoreWebView2Controller> controller;
		composition.As(&controller);
		controller->put_Bounds(RECT{ 0, 0, 640, 480 });
		controller->put_IsVisible(TRUE);
		ComPtr<ICoreWebView2> web;
		controller->get_CoreWebView2(&web);
		EventRegistrationToken token{};
		done = false;
		web->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>([&](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) {
			done = true; return S_OK;
		}).Get(), &token);
		web->NavigateToString(L"<input id='edit'><input id='next'><script>window.events=[];document.addEventListener('keydown',e=>events.push({key:e.key,code:e.code,repeat:e.repeat,ctrl:e.ctrlKey}));edit.focus()</script>");
		PumpUntil([&] { return done; });
		web->remove_NavigationCompleted(token);
		bool failed = false;
		auto queue = std::make_shared<CdpInputQueue>([&](const std::string& method, const nlohmann::json& params, auto complete) {
			const auto hr = web->CallDevToolsProtocolMethod(Wide(method).c_str(), Wide(params.dump()).c_str(),
				Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>([complete, method](HRESULT result, LPCWSTR response) {
					if (FAILED(result)) std::wcerr << L"CDP failed: " << Wide(method) << L" hr=" << std::hex << result << L" " << (response ? response : L"") << L'\n';
					complete(SUCCEEDED(result)); return S_OK;
				}).Get());
			if (FAILED(hr)) complete(false);
		}, [&] { failed = true; });
		auto drain = [&] { PumpUntil([&] { return queue->Idle(); }); Require(!failed, "CDP command failed"); };
		auto script = [&](const wchar_t* expression) {
			std::wstring result;
			bool finished = false;
			web->ExecuteScript(expression, Callback<ICoreWebView2ExecuteScriptCompletedHandler>([&](HRESULT hr, LPCWSTR value) {
				if (SUCCEEDED(hr) && value) result = value;
				finished = true; return S_OK;
			}).Get());
			PumpUntil([&] { return finished; });
			if (result != L"true") {
				std::wcerr << L"Assertion: " << expression << L" -> " << result << L'\n';
				finished = false;
				web->ExecuteScript(L"({a:edit.value,b:next.value,active:document.activeElement.id,focus:document.hasFocus(),events})",
					Callback<ICoreWebView2ExecuteScriptCompletedHandler>([&](HRESULT, LPCWSTR state) {
						std::wcerr << (state ? state : L"null") << L'\n'; finished = true; return S_OK;
					}).Get());
				PumpUntil([&] { return finished; });
			}
			Require(result == L"true", "DOM assertion failed");
		};
		auto key = [&](std::uint32_t vk, const char* name, const char* code, std::uint32_t mods = 0, bool repeat = false) {
			msg::Keyboard event{ .vk = vk, .modifiers = mods, .down = true, .repeat = repeat, .key = name, .code = code };
			queue->Push("Input.dispatchKeyEvent", CdpKeyParams(event));
			event.down = false; event.repeat = false;
			queue->Push("Input.dispatchKeyEvent", CdpKeyParams(event));
		};
		queue->Push("Emulation.setFocusEmulationEnabled", { { "enabled", true } });
		key(0x41, "a", "KeyA");
		queue->Push("Input.dispatchKeyEvent", { { "type", "char" }, { "text", "a" } });
		queue->Push("Input.dispatchKeyEvent", { { "type", "char" }, { "text", "\xC3\xA9\xF0\x9F\x98\x80" } });
		drain();
		script(L"edit.value === 'a'+String.fromCodePoint(0xe9,0x1f600) && document.hasFocus() && events[0].code === 'KeyA'");
		key(VK_BACK, "Backspace", "Backspace", 0, true);
		drain();
		script(L"edit.value === 'a\u00e9' && events.at(-1).repeat === true");
		key(0x41, "a", "KeyA", 2);
		drain();
		script(L"edit.selectionStart === 0 && edit.selectionEnd === 2 && events.at(-1).ctrl");
		queue->Push("Input.dispatchKeyEvent", { { "type", "char" }, { "text", "x" } });
		drain();
		script(L"edit.value === 'x'");
		key(VK_TAB, "Tab", "Tab");
		drain();
		script(L"document.activeElement === next");
		queue->Push("Input.imeSetComposition", { { "text", "\xE6\x97\xA5" }, { "selectionStart", 1 }, { "selectionEnd", 1 } });
		queue->Push("Input.insertText", { { "text", "\xE6\x97\xA5\xE6\x9C\xAC" } });
		drain();
		script(L"next.value === '\u65e5\u672c'");
		queue->Push("Emulation.setFocusEmulationEnabled", { { "enabled", false } });
		drain();
		// Turning off emulation is accepted, but does not necessarily force DOM
		// blur. Production gates input and releases held keys independently.
		DWORD foregroundPid = 0;
		::GetWindowThreadProcessId(::GetForegroundWindow(), &foregroundPid);
		std::cout << "foreground before=" << originalForeground << " after=" << ::GetForegroundWindow()
			<< " foreground pid=" << foregroundPid << " test pid=" << ::GetCurrentProcessId()
			<< " localFocus=" << ::GetFocus() << '\n';
		Require(foregroundPid != ::GetCurrentProcessId() && ::GetFocus() == nullptr, "browser acquired native focus");
		queue->Close();
		controller->Close();
		::DestroyWindow(parent);
		std::cout << "CDP smoke passed: no native focus; text, Unicode, editing, repeat, Ctrl+A, Tab, IME.\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "CDP smoke FAILED: " << error.what() << '\n';
		return 1;
	}
}
