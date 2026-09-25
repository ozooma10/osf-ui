#include "Core/Plugin.h"

#include "API/PapyrusBindHook.h"
#include "Core/Version.h"
#include "REL/THook.h"
#include "Runtime/Runtime.h"

#include <array>
#include <optional>

namespace OSFUI::Plugin
{
	namespace
	{
		constexpr std::array<std::ptrdiff_t, 2> kAdvanceCallSites{ 0x228, 0x2A1 };

		using AdvanceHook = REL::THook<void*(void*, void*, void*, void*)>;

		std::array<std::optional<AdvanceHook>, 2> g_advanceHooks;

		template <std::size_t Index>
		void* AdvanceThunk(void* a_ui, void* a_functor1, void* a_functor2, void* a_arg4) noexcept
		{
			void* result = (*g_advanceHooks[Index])(a_ui, a_functor1, a_functor2, a_arg4);
			Runtime::Get().Update();
			return result;
		}

		bool InstallUiFrameHook()
		{
			g_advanceHooks[0].emplace("FrameTick::Advance0", RE::ID::UI::UpdateMenus, kAdvanceCallSites[0], &AdvanceThunk<0>);
			g_advanceHooks[1].emplace("FrameTick::Advance1", RE::ID::UI::UpdateMenus, kAdvanceCallSites[1], &AdvanceThunk<1>);
			for (auto& hook : g_advanceHooks) {
				if (!hook->Init()) {
					REX::ERROR("FrameTick: UI_AdvanceActiveMenus hook could not be initialized");
					return false;
				}
			}
			for (auto& hook : g_advanceHooks) {
				hook->Enable();
			}
			REX::INFO("FrameTick: installed on both UI_AdvanceActiveMenus call sites in UI::UpdateMenus");
			return true;
		}

		void OnSFSEMessage(SFSE::MessagingInterface::Message* a_msg) noexcept
		{
			if (!a_msg) {
				return;
			}
			switch (a_msg->type) {
				case SFSE::MessagingInterface::kPostLoad:
					REX::INFO("Plugin: SFSE message kPostLoad");
					Runtime::Get().OnPostLoad();
					break;
				case SFSE::MessagingInterface::kPostPostDataLoad:
					REX::INFO("Plugin: SFSE message kPostPostDataLoad");
					Runtime::Get().OnPostPostDataLoad();
					break;
			}
		}
	}

	bool OnLoad() noexcept
	{
		if (!Runtime::Get().Initialize()) {
			REX::ERROR("{}: Runtime initialization failed", kPluginName);
			return false;
		}
		if (const auto* messaging = SFSE::GetMessagingInterface()) {
			if (!messaging->RegisterListener(OnSFSEMessage)) {
				REX::WARN("Plugin: failed to register SFSE message listener (non-fatal)");
			}
		}

		if (!InstallUiFrameHook()) {
			REX::ERROR("{}: frame tick hook unavailable; plugin load aborted", kPluginName);
			return false;
		}
		if (!API::Papyrus::InstallBindHook()) {
			REX::WARN("{}: Papyrus bind hook unavailable; natives bind on the first UI tick after data load instead", kPluginName);
		}

		return true;
	}
}
