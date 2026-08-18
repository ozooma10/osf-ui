#include "Input/PauseMenuEntry.h"

#include "Core/Log.h"

#include "RE/B/BSFixedString.h"
#include "RE/E/Events.h"
#include "REL/Relocation.h"
#include "REL/Trampoline.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace OSFUI
{
	namespace
	{
		constexpr REL::Version kRuntimeVersion{ 1, 16, 244, 0 };
		constexpr REL::ID      kBuilderID{ 93641 };
		constexpr REL::ID      kQueueActionID{ 87656 };
		constexpr REL::ID      kEventSourceID{ 93711 };
		constexpr REL::ID      kExtractActionID{ 93697 };
		constexpr REL::ID      kEventSourceVtableID{ 445619 };

		constexpr std::ptrdiff_t kQuitCallOffset = 0x48D;
		constexpr std::uint32_t  kActionID = 100;

		constexpr std::array<std::uint8_t, 16> kBuilderPrologue{
			0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41,
			0x57, 0x48, 0x8D, 0x6C, 0x24, 0xD1, 0x48, 0x81
		};
		constexpr std::array<std::uint8_t, 16> kQueueActionPrologue{
			0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
			0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55
		};
		constexpr std::array<std::uint8_t, 16> kEventSourcePrologue{
			0x48, 0x83, 0xEC, 0x28, 0x65, 0x48, 0x8B, 0x04,
			0x25, 0x58, 0x00, 0x00, 0x00, 0xBA, 0xB8, 0x00
		};
		constexpr std::array<std::uint8_t, 16> kExtractActionPrologue{
			0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74,
			0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48
		};
		constexpr std::array<std::uint8_t, 6> kQuitCallBytes{
			0xE8, 0x0E, 0x79, 0xE5, 0xFF, 0x90
		};

		using QueueActionFn = void (*)(
			void*, const RE::BSFixedStringCS*, std::uint32_t,
			const RE::BSFixedStringCS*, bool);
		using GetEventSourceFn = RE::BSTEventSource<RE::PauseMenu_StartAction>* (*)();
		using ExtractActionFn = std::uint32_t* (*)(
			std::uint32_t*, const RE::PauseMenu_StartAction*);

		std::atomic<QueueActionFn> g_originalQueueAction{ nullptr };
		ExtractActionFn g_extractAction{ nullptr };
		RE::BSTEventSource<RE::PauseMenu_StartAction>* g_eventSource{ nullptr };
		const RE::BSFixedStringCS* g_label{ nullptr };
		const RE::BSFixedStringCS* g_emptyConfirm{ nullptr };

		std::atomic_bool g_installTried{ false };
		std::atomic_bool g_armed{ false };
		std::atomic_bool g_openRequested{ false };
		std::atomic_bool g_insertionLogged{ false };

		template <std::size_t N>
		bool VerifyBytes(std::string_view a_name, std::uintptr_t a_address,
			const std::array<std::uint8_t, N>& a_expected)
		{
			std::array<std::uint8_t, N> actual{};
			std::memcpy(actual.data(), reinterpret_cast<const void*>(a_address), N);
			if (actual == a_expected) {
				return true;
			}
			REX::ERROR("PauseMenuEntry: {} bytes changed at 0x{:X}; native integration disabled", a_name, a_address);
			return false;
		}

		std::uintptr_t ReadCallTarget(std::uintptr_t a_site)
		{
			std::int32_t displacement = 0;
			std::memcpy(&displacement, reinterpret_cast<const void*>(a_site + 1), sizeof(displacement));
			return a_site + 5 + displacement;
		}

		void QueueActionThunk(
			void* a_model,
			const RE::BSFixedStringCS* a_label,
			std::uint32_t a_actionType,
			const RE::BSFixedStringCS* a_confirmText,
			bool a_disabled)
		{
			const auto original = g_originalQueueAction.load(std::memory_order_acquire);
			if (!original) {
				g_armed.store(false, std::memory_order_release);
				return;
			}

			original(a_model, a_label, a_actionType, a_confirmText, a_disabled);
			if (!g_armed.load(std::memory_order_acquire)) {
				return;
			}

			if ((a_actionType & 0xFF) != 9 || !a_model || !g_label || !g_emptyConfirm) {
				if (g_armed.exchange(false, std::memory_order_acq_rel)) {
					REX::CRITICAL("PauseMenuEntry: native builder invariant failed; integration disarmed for this session");
				}
				return;
			}

			original(a_model, g_label, kActionID, g_emptyConfirm, false);
			if (!g_insertionLogged.exchange(true, std::memory_order_relaxed)) {
				REX::DEBUG("PauseMenuEntry: 'MOD SETTINGS' appended through the native PauseMenu list builder");
			}
		}

		class ActionSink final : public RE::BSTEventSink<RE::PauseMenu_StartAction>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(
				const RE::PauseMenu_StartAction& a_event,
				RE::BSTEventSource<RE::PauseMenu_StartAction>*) override
			{
				if (!g_armed.load(std::memory_order_acquire)) {
					return RE::BSEventNotifyControl::kContinue;
				}

				std::uint32_t action = 0;
				g_extractAction(&action, &a_event);
				if (action != kActionID) {
					return RE::BSEventNotifyControl::kContinue;
				}

				if (!g_openRequested.exchange(true, std::memory_order_acq_rel)) {
					REX::DEBUG("PauseMenuEntry: native action received; queued Mod Settings open");
				}
				return RE::BSEventNotifyControl::kStop;
			}
		};

		ActionSink* Sink()
		{
			static auto* sink = new ActionSink;
			return sink;
		}

		struct Addresses
		{
			std::uintptr_t builder{ 0 };
			std::uintptr_t queueAction{ 0 };
			std::uintptr_t eventSource{ 0 };
			std::uintptr_t extractAction{ 0 };
		};

		bool Preflight(Addresses& a_addresses)
		{
			const auto runtimeVersion = REX::FModule::GetExecutingModule().GetFileVersion();
			if (runtimeVersion != kRuntimeVersion) {
				REX::ERROR("PauseMenuEntry: runtime {} is not {}; native integration disabled",
					runtimeVersion, kRuntimeVersion);
				return false;
			}

			a_addresses.builder = REL::Relocation<std::uintptr_t>{ kBuilderID }.address();
			a_addresses.queueAction = REL::Relocation<std::uintptr_t>{ kQueueActionID }.address();
			a_addresses.eventSource = REL::Relocation<std::uintptr_t>{ kEventSourceID }.address();
			a_addresses.extractAction = REL::Relocation<std::uintptr_t>{ kExtractActionID }.address();
			const auto callsite = a_addresses.builder + kQuitCallOffset;

			if (!VerifyBytes("list builder", a_addresses.builder, kBuilderPrologue) ||
				!VerifyBytes("row helper", a_addresses.queueAction, kQueueActionPrologue) ||
				!VerifyBytes("action source", a_addresses.eventSource, kEventSourcePrologue) ||
				!VerifyBytes("action extractor", a_addresses.extractAction, kExtractActionPrologue) ||
				!VerifyBytes("final QUIT call", callsite, kQuitCallBytes)) {
				return false;
			}
			if (ReadCallTarget(callsite) != a_addresses.queueAction) {
				REX::ERROR("PauseMenuEntry: final QUIT call no longer targets the native row helper; integration disabled");
				return false;
			}

			auto& trampoline = REL::GetTrampoline();
			if (trampoline.empty() || trampoline.free_size() < sizeof(REL::ASM::JMP14)) {
				REX::ERROR("PauseMenuEntry: no trampoline space is available for the builder call hook; integration disabled");
				return false;
			}
			return true;
		}
	}

	bool PauseMenuEntry::Install()
	{
		if (g_installTried.exchange(true, std::memory_order_acq_rel)) {
			return g_armed.load(std::memory_order_acquire);
		}

		Addresses addresses;
		if (!Preflight(addresses)) {
			return false;
		}

		try {
			g_label = new RE::BSFixedStringCS{ "MOD SETTINGS" };
			g_emptyConfirm = new RE::BSFixedStringCS{ "" };
		} catch (const std::exception& e) {
			REX::ERROR("PauseMenuEntry: could not create process-lifetime strings: {}; integration disabled", e.what());
			return false;
		} catch (...) {
			REX::ERROR("PauseMenuEntry: could not create process-lifetime strings; integration disabled");
			return false;
		}

		g_extractAction = reinterpret_cast<ExtractActionFn>(addresses.extractAction);
		g_eventSource = reinterpret_cast<GetEventSourceFn>(addresses.eventSource)();
		if (!g_eventSource) {
			REX::ERROR("PauseMenuEntry: native action source is null; integration disabled");
			return false;
		}

		const auto expectedVtable = REL::Relocation<std::uintptr_t>{ kEventSourceVtableID }.address();
		const auto actualVtable = *reinterpret_cast<const std::uintptr_t*>(g_eventSource);
		if (actualVtable != expectedVtable) {
			REX::ERROR("PauseMenuEntry: native action source identity changed; integration disabled");
			g_eventSource = nullptr;
			return false;
		}
		if (!g_eventSource->sinks.empty()) {
			REX::ERROR("PauseMenuEntry: action source already has {} sink(s); first-listener ordering is unavailable",
				g_eventSource->sinks.size());
			g_eventSource = nullptr;
			return false;
		}

		g_eventSource->RegisterSink(Sink());
		if (g_eventSource->sinks.size() != 1) {
			REX::ERROR("PauseMenuEntry: action sink registration did not produce the required first-listener order");
			g_eventSource->UnregisterSink(Sink());
			g_eventSource = nullptr;
			return false;
		}

		g_originalQueueAction.store(
			reinterpret_cast<QueueActionFn>(addresses.queueAction), std::memory_order_release);
		QueueActionFn captured = nullptr;
		try {
			REL::Relocation<std::uintptr_t> builder{ kBuilderID };
			captured = reinterpret_cast<QueueActionFn>(
				builder.write_call<5, kQuitCallOffset>(&QueueActionThunk));
		} catch (const std::exception& e) {
			REX::ERROR("PauseMenuEntry: builder hook installation failed: {}; integration disabled", e.what());
		} catch (...) {
			REX::ERROR("PauseMenuEntry: builder hook installation failed; integration disabled");
		}
		if (reinterpret_cast<std::uintptr_t>(captured) != addresses.queueAction) {
			REX::CRITICAL("PauseMenuEntry: builder hook captured 0x{:X}, expected 0x{:X}; integration remains disarmed",
				reinterpret_cast<std::uintptr_t>(captured), addresses.queueAction);
			g_eventSource->UnregisterSink(Sink());
			g_eventSource = nullptr;
			return false;
		}

		g_armed.store(true, std::memory_order_release);
		REX::INFO("PauseMenuEntry: native integration armed (action {}; hook and sink are process-lifetime)", kActionID);
		return true;
	}

	bool PauseMenuEntry::TakeOpenRequest()
	{
		return g_openRequested.exchange(false, std::memory_order_acq_rel);
	}
}
