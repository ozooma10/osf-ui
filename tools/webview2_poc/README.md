# OSF UI WebView2 composition/capture POC

This is the standalone Phase 1 spike for a WebView2 renderer. It does not
compile into or modify the Starfield plugin.

## Dependency and build

Use the stable `Microsoft.Web.WebView2` NuGet package version
`1.0.4078.44`. Unpack the package at `external/webview2`, so
`external/webview2/build/native/include/WebView2.h` exists, or set
`WEBVIEW2_SDK_DIR` to the unpacked package root.

```powershell
xmake f -m releasedbg --with_webview2_poc=true
xmake build osfui-webview2-poc
```

The target links `WebView2LoaderStatic.lib`; it does not copy
`WebView2Loader.dll` or a browser runtime. The package is development input
only and `external/` remains gitignored.

## Controls

- F2: inspect the active DOM element and value in the title
- F3: ExecuteScript plus DevTools console probe
- F4: native/web/native bridge latency probe
- F5: load the real settings view
- F6: load a deliberate missing virtual-host resource
- F7: destroy and recreate WebView2/capture resources
- F8: run 20 programmatic focus/restore cycles
- F9: explicitly test the Chrome_WidgetWin fallback
- F10: toggle the overlay
- Esc: close the overlay through AcceleratorKeyPressed

The window title continuously reports stand-in FPS, capture count, staging
readback time, bridge latency, console count, navigation result, cursor ID,
alpha/premultiplication counters, raw input, accelerator events, focus-cycle
result, active-window state, Chrome focus, and the F2 DOM result.

Set `OSFUI_WEBVIEW2_POC_FORCE_RUNTIME_ABSENT=1` to exercise the
runtime-unavailable fallback. The D3D11 stand-in must continue rendering with
zero capture frames.
