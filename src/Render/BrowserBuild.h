#pragma once

#include "BrowserBackend.h"

namespace OSFUI
{
    // Each edition compiles its host choice into the game DLL. Unspecified builds use WebView2.
#if defined(OSFUI_BROWSER_CEF) && OSFUI_BROWSER_CEF
    inline constexpr auto kCompiledBrowserBackend = osfui::browser::Backend::Cef;
#else
    inline constexpr auto kCompiledBrowserBackend = osfui::browser::Backend::WebView2;
#endif
}
