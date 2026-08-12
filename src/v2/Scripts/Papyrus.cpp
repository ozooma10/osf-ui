#include "Papyrus.h"
#include "../Core/Version.h"

namespace Papyrus
{
	inline constexpr std::string_view kPlatformScriptName = "OSFUI";
    
    namespace {
        using PapyrusVM = RE::BSScript::IVirtualMachine;

        std::int32_t GetVersion(PapyrusVM&, std::uint32_t, std::monostate)
        {
            return static_cast<std::int32_t>(Core::kOsfuiReleaseVersionMajor * 10000 + Core::kOsfuiReleaseVersionMinor * 100 + Core::kOsfuiReleaseVersionPatch);
        }

        RE::BSFixedString GetVersionString(PapyrusVM&, std::uint32_t, std::monostate)
        {
            return RE::BSFixedString{ Core::kOsfuiReleaseVersion };
        }

        void BindFunctions(RE::BSScript::IVirtualMachine *a_vm)
        {
            a_vm->BindNativeMethod(kPlatformScriptName, "GetVersion", &GetVersion, true, false);
            a_vm->BindNativeMethod(kPlatformScriptName, "GetVersionString", &GetVersionString, true, false);
        }
    }

    bool RegisterFunctions()
    {
		if (auto* gameVM = RE::GameVM::GetSingleton(); gameVM && gameVM->GetVM()) {
			BindFunctions(gameVM->GetVM());
            REX::INFO("Papyrus: registered version natives on script '{}'", kPlatformScriptName);
			return true;
		}
		return false;
    }

}
