#pragma once
#include "OSFUI_SurfaceAudio.h"

namespace OSFUI::ScreenAudio
{
    // Only inside NativeMainThreadQueue's verified drain. Engine pointers never
    // escape; the returned scene owns a copy of all render-space coordinates.
    API::Surfaces::AudioScene Capture();
}
