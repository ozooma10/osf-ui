#include "Core/Plugin.h"

#include "Core/Version.h"
#include "REL/Trampoline.h"
#include "Runtime/Runtime.h"

#include <array>
#include <cstring>

namespace OSFUI::Plugin
{
	namespace
	{
		// Starfield 1.16.244: UI::UpdateMenus calls UI_AdvanceActiveMenus from exactly one of two
		// mutually exclusive sites per frame, on the game main thread. Hooking both gives a once-per-frame
		// main-thread pump with no SFSE worker hop. Same design as CruiseFromStarmap's UiPostAdvanceHook.
		constexpr std::array<std::ptrdiff_t, 2> kAdvanceCallSites{ 0x228, 0x2A1 };
		constexpr auto                          kIdleTickInterval = std::chrono::milliseconds(100);
		constexpr double                        kMaxDeltaSeconds = 0.1;

		using AdvanceFn = void* (*)(void*, void*, void*, void*);

		AdvanceFn                                            g_advance{ nullptr };
		std::optional<std::chrono::steady_clock::time_point> g_lastTick;
		std::chrono::steady_clock::time_point                g_nextIdleTick{};

		void OnUiFrame()
		{
			const auto now = std::chrono::steady_clock::now();
			if (!Runtime::Get().IsVisible() && now < g_nextIdleTick) {
				return;
			}
			g_nextIdleTick = now + kIdleTickInterval;

			double dt = g_lastTick ? std::chrono::duration<double>(now - *g_lastTick).count() : 0.0;
			g_lastTick = now;
			dt = std::clamp(dt, 0.0, kMaxDeltaSeconds);
			Runtime::Get().Tick(dt);
		}

		void* AdvanceThunk(void* a_ui, void* a_functor1, void* a_functor2, void* a_arg4)
		{
			void* result = g_advance(a_ui, a_functor1, a_functor2, a_arg4);
			try {
				OnUiFrame();
			} catch (const std::exception& e) {
				REX::ERROR("FrameTick: Runtime::Tick threw '{}'", e.what());
			} catch (...) {
				REX::ERROR("FrameTick: Runtime::Tick threw an unknown exception");
			}
			return result;
		}

		std::uintptr_t DecodeCallTarget(std::uintptr_t a_site)
		{
			if (*reinterpret_cast<const std::uint8_t*>(a_site) != 0xE8) {
				return 0;
			}
			std::int32_t rel{};
			std::memcpy(&rel, reinterpret_cast<const void*>(a_site + 1), sizeof(rel));
			return a_site + 5 + rel;
		}

		bool InstallUiFrameHook()
		{
			const auto base = RE::ID::UI::UpdateMenus.address();
			const auto target = DecodeCallTarget(base + kAdvanceCallSites[0]);
			if (!target || target != DecodeCallTarget(base + kAdvanceCallSites[1])) {
				REX::ERROR("FrameTick: UI_AdvanceActiveMenus call sites in UI::UpdateMenus do not match the expected layout; frame tick unavailable");
				return false;
			}
			// Call whatever target is there now so hooks from other plugins on the same sites stay chained.
			g_advance = reinterpret_cast<AdvanceFn>(target);
			for (const auto offset : kAdvanceCallSites) {
				REL::GetTrampoline().write_call<5>(base + offset, &AdvanceThunk);
			}
			REX::INFO("FrameTick: installed on both UI_AdvanceActiveMenus call sites in UI::UpdateMenus");
			return true;
		}

		void OnSFSEMessage(SFSE::MessagingInterface::Message* a_msg)
		{
			if (!a_msg) {
				return;
			}
			try {
				switch (a_msg->type) {
					case SFSE::MessagingInterface::kPostPostLoad:
						REX::INFO("Plugin: SFSE message kPostPostLoad");
						Runtime::Get().OnPostPostLoad();
						break;
					case SFSE::MessagingInterface::kPostDataLoad:
						REX::INFO("Plugin: SFSE message kPostDataLoad");
						Runtime::Get().OnDataLoaded();
						break;
					case SFSE::MessagingInterface::kPostPostDataLoad:
						REX::INFO("Plugin: SFSE message kPostPostDataLoad");
						Runtime::Get().OnPostDataLoaded();
						break;
				}
			} catch (const std::exception& e) {
				REX::ERROR("Plugin: SFSE message {} threw '{}'", a_msg->type, e.what());
			} catch (...) {
				REX::ERROR("Plugin: SFSE message {} threw an unknown exception", a_msg->type);
			}
		}
	}

	bool OnLoad()
	{
		try {
			if (!Runtime::Get().Initialize()) {
				REX::ERROR("{}: Runtime initialization failed", kPluginName);
				return false;
			}
		} catch (const std::exception& e) {
			REX::ERROR("{}: Runtime initialization threw '{}'; plugin load aborted", kPluginName, e.what());
			return false;
		} catch (...) {
			REX::ERROR("{}: Runtime initialization threw an unknown exception; plugin load aborted", kPluginName);
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

		return true;
	}
}
