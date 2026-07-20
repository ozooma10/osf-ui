#include "pch.h"

// The out-of-process WebView2 renderer shares its pipe/broker-launch code with
// the standalone host tools (tools/webview2_shared). Those sources are pch-free;
// including them through this wrapper satisfies the plugin's /Yu requirement
// without forking them. Empty when the backend is not compiled in.
#if defined(OSFUI_WITH_WEBVIEW2)
#	include "../../tools/webview2_shared/Wv2Pipe.cpp"
#	include "../../tools/webview2_shared/Wv2BrokerLaunch.cpp"
#endif
