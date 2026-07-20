#include "input/UiLayoutGuard.h"

#include "RE/B/BSInputEventReceiver.h"
#include "RE/U/UI.h"

namespace OSFUI
{
	namespace
	{
		// Index of the BSInputEventReceiver vtable in RE::UI::VTABLE (AddressLib
		// ID 475439), confirmed on game 1.16.244. VerifyUiLayout() hard-fails if
		// this stops holding.
		constexpr std::size_t kReceiverVtblIndex = 10;
	}

	bool UiLayoutGuard::VerifyUiLayout()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			REX::ERROR("UiLayoutGuard: RE::UI singleton is null; layout unverifiable");
			return false;
		}

		// Cross-check compiled UI base offsets against the running binary before
		// anything writes to or registers on the UI object: the live
		// BSInputEventReceiver subobject's vptr must equal the vtable AddressLib
		// reports. A stale CommonLibSF layout then fails here instead of
		// corrupting UI state — that shipped against 1.16.244 with a pre-PR#26
		// submodule and crashed on save load.
		//
		// The receiver's entry is VTABLE[kReceiverVtblIndex], not VTABLE[0] or [1];
		// IDs_VTABLE.h array order does not follow base-declaration order. Derived
		// 2026-06-12 by resolving all 11 versionlib-1-16-244 entries
		// (tools/parse_versionlib.py) against the live vptr; the match is also the
		// cluster's only 2-slot vtable (dtor + PerformInputProcessing), which is
		// BSInputEventReceiver's shape. Both checks below stay hard requirements:
		// a reordered array or a moved vtable refuses and dumps what re-derivation
		// needs.
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
