#pragma once
#ifdef OSF_UI_PASS_DIRECT_PROBE
#include "Composite/D3D12Prologue.h"
namespace OSFUI::UiPass::StateAudit
{
    bool Install(void** table);
    void Begin();
    void End(std::uintptr_t listIdentity, unsigned frame, bool detail);
    void Phase(const char* name);
    void Foreign(bool active);
}
#endif
