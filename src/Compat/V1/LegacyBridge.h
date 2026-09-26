#pragma once
#include "Compat/V1/LegacyABI.h"

namespace OSFUI::Compat::V1
{
    IOSFUIBridge& Bridge();
    void Initialize();
    void Pump();
}
