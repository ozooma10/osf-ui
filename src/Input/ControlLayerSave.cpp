#include "Input/ControlLayer.h"

#include "RE/B/BGSSaveLoad.h"
#include "RE/E/Events.h"
#include "REL/THook.h"

#include <array>
#include <cstring>

namespace OSFUI
{
	namespace
	{
		std::optional<REL::THook<void()>> g_pumpHook;

		void Pump()
		{
			const auto* manager = RE::BGSSaveLoadManager::GetSingleton();
			// Game.RequestSave (bit 1) omits SaveLoadEvent begin. Bracket the
			// synchronous main-thread pump, leaving Animation's inner save hook intact.
			const bool silent = manager && manager->queuedTasks.any(RE::BGSSaveLoadManager::QueuedTask::kForceSave);
			if (silent) ControlLayer::BeginRequestedSave();
			struct Resume { bool active; ~Resume() { if (active) ControlLayer::EndRequestedSave(); } } resume{silent};
			(*g_pumpHook)();
		}

		class Sink final : public RE::BSTEventSink<RE::SaveLoadEvent>, public RE::BSTEventSink<RE::TESLoadGameEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::SaveLoadEvent& event, RE::BSTEventSource<RE::SaveLoadEvent>*) override
			{
				using Op = RE::SaveLoadEvent::OpType;
				using Status = RE::SaveLoadEvent::Status;
				const bool save = event.opType == Op::kAutosave || event.opType == Op::kQuicksave ||
					event.opType == Op::kManualSave || event.opType == Op::kExitSaveToMainMenu || event.opType == Op::kExitSaveToDesktop;
				const bool load = event.opType == Op::kLoadMostRecent || event.opType == Op::kQuickload ||
					event.opType == Op::kLoad || event.opType == Op::kLoadNamedFile;
				if (save) ControlLayer::SaveWindow(event.status == Status::kBegin);
				if (load) {
					if (event.status == Status::kBegin) ControlLayer::BeginLoad();
					else if (event.status == Status::kFailed || event.status == Status::kLoadDispatchRefused) ControlLayer::EndLoad();
				}
				return RE::BSEventNotifyControl::kContinue;
			}
			RE::BSEventNotifyControl ProcessEvent(const RE::TESLoadGameEvent&, RE::BSTEventSource<RE::TESLoadGameEvent>*) override
			{
				ControlLayer::EndLoad();
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	bool ControlLayer::InstallSaveSafety()
	{
		static const bool installed = [] {
			auto* saves = RE::SaveLoadEvent::GetEventSource();
			auto* loads = RE::TESLoadGameEvent::GetEventSource();
			if (!saves || !loads) return false;
			// 1.16.244: Main update 99392+0x205 calls request pump 98691 with no
			// arguments; the next instruction reads only the preserved r14 register.
			// Verify the original destination and following instruction before patching.
			constexpr REL::ID caller{99392}, pump{98691};
			constexpr std::ptrdiff_t offset = 0x205;
			const auto site = caller.address() + offset;
			constexpr std::array<std::uint8_t, 8> following{0x41,0x80,0xBE,0x5A,0x04,0x00,0x00,0x00};
			if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8 ||
				REL::ASM::CALL5::TARGET(site) != pump.address() ||
				std::memcmp(reinterpret_cast<const void*>(site + 5), following.data(), following.size()) != 0) {
				REX::ERROR("ControlLayer: save-safety hook differs from verified 1.16.244; input capture refused");
				return false;
			}
			g_pumpHook.emplace("ControlLayer::SavePump", caller, offset, &Pump);
			if (!g_pumpHook->Init() || !g_pumpHook->Enable()) return false;
			static auto* sink = new Sink;
			saves->RegisterSink(sink);
			loads->RegisterSink(sink);
			REX::INFO("ControlLayer: save/load safety installed");
			return true;
		}();
		return installed;
	}
}
