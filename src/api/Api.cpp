#include "api/Api.h"

#include "runtime/Runtime.h"

// PrismaUI SF public consumer API.
//
// Each method validates its arguments and delegates to Runtime, wrapping the
// consumer's C callbacks (raw function pointers) into the std::function shapes
// Runtime/the renderer expect. No extra thread marshaling is done here: the
// renderer already delivers every callback (DOM-ready, Invoke result, JS
// listener, console) on the game (main) thread via its Update() drain, so the
// consumer's callback runs on the main thread exactly as with PrismaUI.

namespace PrismaSF
{
	PrismaView PluginAPI::PrismaUIInterface::CreateView(const char* htmlPath,
		PRISMA_UI_API::OnDomReadyCallback onDomReadyCallback) noexcept
	{
		if (!htmlPath) {
			return 0;
		}
		ViewRegistry::DomReadyCb wrapped;
		if (onDomReadyCallback) {
			wrapped = [onDomReadyCallback](std::uint64_t a_handle) { onDomReadyCallback(a_handle); };
		}
		return Runtime::Get().ApiCreateView(htmlPath, std::move(wrapped));
	}

	void PluginAPI::PrismaUIInterface::Invoke(PrismaView view, const char* script,
		PRISMA_UI_API::JSCallback callback) noexcept
	{
		if (!view || !script) {
			return;
		}
		IWebRenderer::ScriptResultHandler onResult;
		if (callback) {
			onResult = [callback](std::string a_result) { callback(a_result.c_str()); };
		}
		Runtime::Get().ApiInvoke(view, script, std::move(onResult));
	}

	void PluginAPI::PrismaUIInterface::InteropCall(PrismaView view, const char* functionName,
		const char* argument) noexcept
	{
		if (!view || !functionName || !argument) {
			return;
		}
		Runtime::Get().ApiInteropCall(view, functionName, argument);
	}

	void PluginAPI::PrismaUIInterface::RegisterJSListener(PrismaView view, const char* functionName,
		PRISMA_UI_API::JSListenerCallback callback) noexcept
	{
		if (!view || !functionName || !callback) {
			return;
		}
		IWebRenderer::JsListenerHandler handler = [callback](std::string a_arg) { callback(a_arg.c_str()); };
		Runtime::Get().ApiRegisterJSListener(view, functionName, std::move(handler));
	}

	bool PluginAPI::PrismaUIInterface::HasFocus(PrismaView view) noexcept
	{
		return view && Runtime::Get().ApiHasFocus(view);
	}

	bool PluginAPI::PrismaUIInterface::Focus(PrismaView view, bool pauseGame, bool disableFocusMenu) noexcept
	{
		return view && Runtime::Get().ApiFocus(view, pauseGame, disableFocusMenu);
	}

	void PluginAPI::PrismaUIInterface::Unfocus(PrismaView view) noexcept
	{
		if (view) {
			Runtime::Get().ApiUnfocus(view);
		}
	}

	void PluginAPI::PrismaUIInterface::Show(PrismaView view) noexcept
	{
		if (view) {
			Runtime::Get().ApiShow(view);
		}
	}

	void PluginAPI::PrismaUIInterface::Hide(PrismaView view) noexcept
	{
		if (view) {
			Runtime::Get().ApiHide(view);
		}
	}

	bool PluginAPI::PrismaUIInterface::IsHidden(PrismaView view) noexcept
	{
		return !view || Runtime::Get().ApiIsHidden(view);
	}

	int PluginAPI::PrismaUIInterface::GetScrollingPixelSize(PrismaView view) noexcept
	{
		return view ? Runtime::Get().ApiGetScrollingPixelSize(view) : 0;
	}

	void PluginAPI::PrismaUIInterface::SetScrollingPixelSize(PrismaView view, int pixelSize) noexcept
	{
		if (view) {
			Runtime::Get().ApiSetScrollingPixelSize(view, pixelSize);
		}
	}

	bool PluginAPI::PrismaUIInterface::IsValid(PrismaView view) noexcept
	{
		return view && Runtime::Get().ApiIsValid(view);
	}

	void PluginAPI::PrismaUIInterface::Destroy(PrismaView view) noexcept
	{
		if (view) {
			Runtime::Get().ApiDestroy(view);
		}
	}

	void PluginAPI::PrismaUIInterface::SetOrder(PrismaView view, int order) noexcept
	{
		if (view) {
			Runtime::Get().ApiSetOrder(view, order);
		}
	}

	int PluginAPI::PrismaUIInterface::GetOrder(PrismaView view) noexcept
	{
		return view ? Runtime::Get().ApiGetOrder(view) : -1;
	}

	void PluginAPI::PrismaUIInterface::CreateInspectorView(PrismaView) noexcept
	{
		static bool warned = false;
		if (!warned) {
			warned = true;
			REX::WARN("PrismaUI SF: Inspector/DevTools is not implemented yet; "
					  "CreateInspectorView and the other inspector calls are no-ops");
		}
	}

	void PluginAPI::PrismaUIInterface::SetInspectorVisibility(PrismaView, bool) noexcept {}

	bool PluginAPI::PrismaUIInterface::IsInspectorVisible(PrismaView) noexcept
	{
		return false;
	}

	void PluginAPI::PrismaUIInterface::SetInspectorBounds(PrismaView, float, float, unsigned int,
		unsigned int) noexcept
	{
	}

	bool PluginAPI::PrismaUIInterface::HasAnyActiveFocus() noexcept
	{
		return Runtime::Get().ApiHasAnyActiveFocus();
	}

	void PluginAPI::PrismaUIInterface::RegisterConsoleCallback(PrismaView view,
		PRISMA_UI_API::ConsoleMessageCallback callback) noexcept
	{
		if (!view) {
			return;
		}
		ViewRegistry::ConsoleCb wrapped;
		if (callback) {
			wrapped = [callback, view](int a_level, std::string a_message) {
				callback(view, static_cast<PRISMA_UI_API::ConsoleMessageLevel>(a_level), a_message.c_str());
			};
		}
		Runtime::Get().ApiRegisterConsoleCallback(view, std::move(wrapped));
	}
}

// Exported entry point. Consumers resolve this via GetProcAddress (see
// PrismaUI_API.h::RequestPluginAPI). NOT an SFSE message handler.
extern "C" __declspec(dllexport) void* RequestPluginAPI(const PRISMA_UI_API::InterfaceVersion a_interfaceVersion)
{
	auto* api = PrismaSF::PluginAPI::PrismaUIInterface::GetSingleton();
	switch (a_interfaceVersion) {
		case PRISMA_UI_API::InterfaceVersion::V1:
			REX::INFO("RequestPluginAPI: returning IVPrismaUI1 (V1)");
			return static_cast<PRISMA_UI_API::IVPrismaUI1*>(api);
		case PRISMA_UI_API::InterfaceVersion::V2:
			REX::INFO("RequestPluginAPI: returning IVPrismaUI2 (V2)");
			return static_cast<PRISMA_UI_API::IVPrismaUI2*>(api);
		default:
			REX::WARN("RequestPluginAPI: unsupported interface version requested");
			return nullptr;
	}
}
