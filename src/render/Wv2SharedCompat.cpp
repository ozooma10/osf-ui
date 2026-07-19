#include "pch.h"

// The out-of-process WebView2 host renderer shares its pipe/broker-launch
// implementation with the standalone host tools (tools/webview2_shared).
// Those sources are plain, pch-free TUs; compiling them here through this
// wrapper keeps the plugin's /Yu pch requirement satisfied without forking
// the code. Empty when the backend is not compiled in.
#if defined(OSFUI_WITH_WEBVIEW2)
#	include "../../tools/webview2_shared/Wv2Pipe.cpp"
#	include "../../tools/webview2_shared/Wv2BrokerLaunch.cpp"
#endif
