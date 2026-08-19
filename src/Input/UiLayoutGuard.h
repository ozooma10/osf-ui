#pragma once

// Fail closed unless the live RE::UI BSInputEventReceiver vptr matches VTABLE[10] (ID 475439).

namespace OSFUI
{
	class UiLayoutGuard
	{
	public:
		// Call on the first main-thread tick; no RE::UI access is safe until this passes.
		static bool VerifyUiLayout();
	};
}
