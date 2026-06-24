# Consumer API — driving PrismaUI SF from your own SFSE plugin

PrismaUI SF exposes a native C++ API so **other SFSE plugins** can create and
drive HTML/CSS/JS views at runtime — the PrismaUI (Skyrim) developer model,
ported to Starfield. The interface is **source-compatible with PrismaUI**: the
namespace (`PRISMA_UI_API`), the `PrismaView` handle, and every method signature
match `PrismaUI_API.h` byte-for-byte. Porting a PrismaUI consumer to Starfield
is little more than swapping the header and the DLL name.

> This is the **trusted native consumer** path. The JS sandbox
> ([security-model.md](security-model.md)) governs *web content*; a consumer is
> a native plugin the user installed, so it gets the full API (arbitrary
> `Invoke`, JS listeners, …). It coexists with the declarative on-disk-view +
> `window.prisma` model — neither replaces the other.

## 1. Get the API

Copy [`sdk/PrismaUI_API.h`](../sdk/PrismaUI_API.h) into your project and request
the interface **after SFSE's `kPostLoad`** message (so the PrismaUI SF DLL is
loaded). `RequestPluginAPI` resolves a plain exported function via
`GetModuleHandle(L"PrismaUI SF.dll") + GetProcAddress` — it is **not** SFSE
messaging.

```cpp
#include "PrismaUI_API.h"

static PRISMA_UI_API::IVPrismaUI1* g_prisma = nullptr;

void OnMessage(SKSE::MessagingInterface::Message* m) {   // or the SFSE equivalent
    if (m->type == MessagingInterface::kPostLoad) {
        g_prisma = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI1>();
        if (!g_prisma) { /* PrismaUI SF not installed or too old */ return; }
        // ... create your view here
    }
}
```

Use `IVPrismaUI2` for `RegisterConsoleCallback` (V2). Request the version you
need; a `nullptr` return means that version is unavailable.

## 2. Create a view

```cpp
// htmlPath is "<folder>/<file>" under Data/SFSE/Plugins/PrismaUI/views/.
PrismaView view = g_prisma->CreateView("MyMod/index.html", [](PrismaView v) {
    // Runs on the game main thread once the DOM is ready — safe to talk to JS.
    g_prisma->Invoke(v, "initUI()");
});
```

Ship your view as `Data/SFSE/Plugins/PrismaUI/views/MyMod/index.html` (+ css/js).
`CreateView` returns a handle (0 on failure) immediately; the view loads on the
renderer's worker and the callback fires when its DOM is ready.

## 3. Talk to JavaScript

| Direction | How |
|---|---|
| native → JS (eval) | `Invoke(view, "anyJavaScript()", optionalResultCallback)` — runs arbitrary JS; the optional callback receives the expression's result as a string. |
| native → JS (fast) | `InteropCall(view, "fnName", "argString")` — calls `window.fnName("argString")` directly, no eval/parse. |
| JS → native | `RegisterJSListener(view, "onSave", cb)` exposes `window.onSave(str)` in the page; calling it fires `cb(const char* str)` on the game main thread. |
| console | (V2) `RegisterConsoleCallback(view, cb)` delivers the view's `console.log/warn/error/...` with a level. |

```cpp
g_prisma->RegisterJSListener(view, "onSave", [](const char* data) {
    SFSE::log::info("page saved: {}", data);
});
g_prisma->Invoke(view, "document.title", [](const char* title) {
    SFSE::log::info("title is {}", title);
});
```

All consumer callbacks run on the **game main thread**, so reading game state
inside them is safe.

## 4. Focus, visibility, order, lifecycle

| Method | Effect |
|---|---|
| `Focus(view, pauseGame=false, disableFocusMenu=false)` | Show the overlay, make this view active, and capture keyboard/mouse. `pauseGame` engages the input-disable layer (experimental); `disableFocusMenu` skips the engine focus menu. |
| `Unfocus(view)` | Release input focus (and any `pauseGame` freeze). |
| `HasFocus(view)` / `HasAnyActiveFocus()` | Query focus state. |
| `Show(view)` / `Hide(view)` / `IsHidden(view)` | Per-view visibility within the overlay. |
| `SetOrder(view, n)` / `GetOrder(view)` | Z-order among views (higher draws on top). |
| `SetScrollingPixelSize(view, px)` / `GetScrollingPixelSize(view)` | Scroll step. |
| `IsValid(view)` | Is the handle still live. |
| `Destroy(view)` | Tear the view down and free its resources. |

## Threading

Call the API from the game main thread (an SFSE message handler or a per-frame
task), as PrismaUI consumers do. Queries (`IsValid`, `IsHidden`, `GetOrder`, …)
return immediately; mutations take effect on the renderer's next worker pass;
callbacks are delivered on the main thread.

## Not yet implemented

- **Inspector / DevTools** (`CreateInspectorView`, `SetInspector*`) — present for
  ABI compatibility but currently logged no-ops. A real inspector is planned.
- **Mouse-wheel scrolling** into views is a separate work item; `SetScrollingPixelSize`
  stores the step but wheel routing is not wired yet.
- `Focus(pauseGame=true)` and `disableFocusMenu` ride on experimental engine
  integration (`ControlLayer` / `FocusMenu`) still being proven on 1.16.244.

## Versioning

`PrismaUI_API.h` carries `InterfaceVersion { V1, V2 }`. New capability is added as
a new `IVPrismaUI<N>` that extends the previous one, so existing consumers keep
working. Request the highest version you need and handle a `nullptr` (older
PrismaUI SF) gracefully.
