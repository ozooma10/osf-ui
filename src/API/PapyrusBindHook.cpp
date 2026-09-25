#include "API/PapyrusBindHook.h"

#include "API/PapyrusApi.h"
#include "REL/THook.h"

#include <optional>

namespace OSFUI::API::Papyrus
{
	namespace
	{
		using VM = RE::BSScript::IVirtualMachine;

		// Starfield 1.16.244: GameVM's constructor calls BindEverythingToScript(&vm) at +0x802, once the VM exists
		// and before any script runs. Binding there gives one deterministic bind per process. Same site as OSF Settings.
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
		if (g_bindHook) {
			return g_bindHook->GetEnabled();
		}
		const auto site = RE::ID::GameVM::Ctor.address() + kBindCallOffset;
		if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8 ||
			REL::ASM::CALL5::TARGET(site) != RE::ID::GameVM::BindEverythingToScript.address()) {
			REX::ERROR("PapyrusApi: GameVM constructor does not call BindEverythingToScript at the expected site; bind hook not installed");
			return false;
		}
		g_bindHook.emplace("PapyrusApi::Bind", RE::ID::GameVM::Ctor, kBindCallOffset, &BindEverythingToScript);
		if (!g_bindHook->Init() || !g_bindHook->Enable()) {
			REX::ERROR("PapyrusApi: bind hook could not be enabled");
			g_bindHook.reset();
			return false;
		}
		REX::INFO("PapyrusApi: natives will bind from GameVM's constructor");
		return true;
	}
}
