#include "Input/UiLayoutGuard.h"

#include "RE/B/BSInputEventReceiver.h"
#include "RE/U/UI.h"

namespace OSFUI
{
	namespace
	{
		// BSInputEventReceiver is RE::UI::VTABLE[10] on 1.16.244 (AddressLib ID 475439).
		constexpr std::size_t kReceiverVtblIndex = 10;
	}

	bool UiLayoutGuard::VerifyUiLayout()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			REX::ERROR("UiLayoutGuard: RE::UI singleton is null; layout unverifiable");
			return false;
		}

		// Compare the live receiver subobject against AddressLib before any UI registration or write.
		auto* receiver = static_cast<RE::BSInputEventReceiver*>(ui);
		const auto liveVptr = *reinterpret_cast<const std::uintptr_t*>(receiver);
		const REL::Relocation<std::uintptr_t> vtbl{ RE::UI::VTABLE[kReceiverVtblIndex] };
		if (liveVptr != vtbl.address()) {
			REX::ERROR(
				"UiLayoutGuard: UI layout guard FAILED — live BSInputEventReceiver vptr {:#x} != AddressLib UI::VTABLE[{}] {:#x} "
				"(CommonLibSF layout or address library stale for this game version); dumping all entries:",
				liveVptr, kReceiverVtblIndex, vtbl.address());
			for (std::size_t i = 0; i < RE::UI::VTABLE.size(); ++i) {
				const REL::Relocation<std::uintptr_t> entry{ RE::UI::VTABLE[i] };
				REX::ERROR("UiLayoutGuard:   UI::VTABLE[{:2}] (ID {}) = {:#x}{}",
					i, RE::UI::VTABLE[i].id(), entry.address(),
					entry.address() == liveVptr ? "  <-- matches live vptr" : "");
			}
			return false;
		}
		return true;
	}
}
