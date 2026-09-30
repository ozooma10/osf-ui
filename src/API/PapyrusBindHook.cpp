#include "API/PapyrusBindHook.h"

#include "API/PapyrusApi.h"
#include "REL/THook.h"

#include <optional>

namespace OSFUI::API::Papyrus
{
	namespace
	{
		using VM = RE::BSScript::IVirtualMachine;

		constexpr std::ptrdiff_t kBindCallOffset = 0x802;

		std::optional<REL::THook<void(VM**)>> g_bindHook;

		void BindEverythingToScript(VM** a_vm)
		{
			(*g_bindHook)(a_vm);
			if (a_vm && *a_vm) {
				BindNatives(**a_vm);
			}
		}
	}

	bool InstallBindHook()
	{
		g_bindHook.emplace("PapyrusApi::Bind", RE::ID::GameVM::Ctor, kBindCallOffset, &BindEverythingToScript);
		if (!g_bindHook->Init()) {
			REX::ERROR("PapyrusApi: bind hook could not be initialized");
			return false;
		}
		g_bindHook->Enable();
		REX::INFO("PapyrusApi: natives will bind from GameVM's constructor");
		return true;
	}
}
