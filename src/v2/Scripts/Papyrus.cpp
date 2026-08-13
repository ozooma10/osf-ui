#include "Papyrus.h"
#include "../Core/Version.h"
#include "core/StringUtil.h"

#include "RE/B/BSScriptUtil.h"

#include <atomic>
#include <cstdint>
#include <string_view>
#include <variant>

namespace Papyrus
{
	inline constexpr std::string_view kPlatformScriptName = "OSFUI";

	namespace
	{
		using PapyrusVM = RE::BSScript::IVirtualMachine;

		std::atomic<ViewRequestHandler> g_openViewHandler{ nullptr };
		std::atomic<ViewRequestHandler> g_closeViewHandler{ nullptr };

		std::int32_t GetVersion(PapyrusVM&, std::uint32_t, std::monostate)
		{
			return static_cast<std::int32_t>(Core::kOsfuiReleaseVersionMajor * 10000 + Core::kOsfuiReleaseVersionMinor * 100 + Core::kOsfuiReleaseVersionPatch);
		}

		RE::BSFixedString GetVersionString(PapyrusVM&, std::uint32_t, std::monostate)
		{
			return RE::BSFixedString{ Core::kOsfuiReleaseVersion };
		}

		bool OpenMenu(PapyrusVM&, std::uint32_t, std::monostate, RE::BSFixedString a_viewId)
		{
			const auto handler = g_openViewHandler.load(std::memory_order_acquire);
			return handler && handler(OSFUI::StringUtil::ToLowerAscii(a_viewId.c_str()));
		}

		bool CloseMenu(PapyrusVM&, std::uint32_t, std::monostate, RE::BSFixedString a_viewId)
		{
			const auto handler = g_closeViewHandler.load(std::memory_order_acquire);
			return handler && handler(OSFUI::StringUtil::ToLowerAscii(a_viewId.c_str()));
		}

		void BindFunctions(RE::BSScript::IVirtualMachine* a_vm)
		{
			a_vm->BindNativeMethod(kPlatformScriptName, "GetVersion", &GetVersion, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "GetVersionString", &GetVersionString, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "OpenMenu", &OpenMenu, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "CloseMenu", &CloseMenu, true, false);
		}
	}

	void SetViewRequestHandlers(ViewRequestHandler a_openView, ViewRequestHandler a_closeView)
	{
		g_openViewHandler.store(a_openView, std::memory_order_release);
		g_closeViewHandler.store(a_closeView, std::memory_order_release);
	}

	bool RegisterFunctions()
	{
		if (auto* gameVM = RE::GameVM::GetSingleton(); gameVM && gameVM->GetVM()) {
			BindFunctions(gameVM->GetVM());
			REX::INFO("Papyrus: registered platform natives on script '{}'", kPlatformScriptName);
			return true;
		}
		return false;
	}
}
