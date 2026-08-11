#include "Papyrus.h"

namespace Papyrus
{
	inline constexpr std::string_view kPlatformScriptName = "OSFUI";
    
    
    namespace {
        void RegisterFunctions(RE::BSScript::IVirtualMachine *a_vm)
        {
            //
        }
    }

    bool RegisterFunctions()
    {
		if (auto* gameVM = RE::GameVM::GetSingleton(); gameVM && gameVM->GetVM()) {
			RegisterFunctions(gameVM->GetVM());
			return true;
		}
		return false;
    }

} // namespace Papyrus
