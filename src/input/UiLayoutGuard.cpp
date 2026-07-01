#include "input/UiLayoutGuard.h"

#include "RE/B/BSInputEventReceiver.h"
#include "RE/U/UI.h"

namespace OSFUI
{
	namespace
	{
		// Index of the BSInputEventReceiver vtable in RE::UI::VTABLE
		// (AddressLib ID 475439). Proven on game 1.16.244 — see
		// VerifyUiLayout(), which hard-fails if this stops being true.
		constexpr std::size_t kReceiverVtblIndex = 10;
	}

	bool UiLayoutGuard::VerifyUiLayout()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			REX::ERROR("UiLayoutGuard: RE::UI singleton is null; layout unverifiable");
			return false;
		}

		// Cross-check the compiled UI base offsets against the running binary
		// before anything writes to or registers on the UI object: the vptr of
		// the live BSInputEventReceiver subobject must be exactly the vtable
		// AddressLib reports for it. A stale CommonLibSF layout fails this
		// instead of corrupting UI state — that exact failure shipped against
		// game 1.16.244 with a pre-PR#26 submodule and crashed on save load.
		//
		// The receiver's entry is VTABLE[kReceiverVtblIndex], NOT VTABLE[0] or
		// [1]: the IDs_VTABLE.h array order does not follow base-declaration
		// order. Proven 2026-06-12 by resolving all 11 entries from
		// versionlib-1-16-244 (tools/parse_versionlib.py) against the vptr
		// observed in the running game; the matched vtable is also the only
		// 2-slot one in the cluster (dtor + PerformInputProcessing), which is
		// exactly BSInputEventReceiver's shape. Both checks below stay hard
		// requirements: if CommonLibSF reorders the array or a patch moves the
		// vtable, this refuses and dumps the data needed to re-derive.
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
