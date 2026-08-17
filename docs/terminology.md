# OSF UI terminology

This glossary is the canonical vocabulary for current OSF UI documentation and
new code. Public API fields, protocol values, manifest keys, CLI flags, and
installed paths shown in backticks keep their existing spelling for
compatibility; the definitions below say what those spellings mean.

Historical design records, migration guides, compatibility code, and changelog
entries intentionally retain the vocabulary of the release they describe.

## Components and processes

| Term | Meaning |
|---|---|
| **OSF UI runtime** | `OSFUI.dll` and its in-game `Runtime` orchestration. Do not shorten this to *host* when the browser host could also be meant. |
| **browser host** | `osfui_webview2_host.exe`, the out-of-process executable that owns WebView2 environments and composition controllers. |
| **shared bridge helper** | `views/shared/osfui.js`, the browser-side JavaScript library that exposes `window.osfui`, manages request correlation, and consumes bridge envelopes. It is not the browser host. |
| **web renderer** | The game-side `IWebRenderer` implementation (`WebView2HostWebRenderer`) that communicates with the browser host. |
| **compositor** | `D3D12Compositor`, which draws browser-host textures in Starfield's UI pass. |
| **mod backend** | Papyrus or a native SFSE plugin that owns game logic and exchanges state, events, sends, and requests with its views. It is not the web renderer. |
| **WebView2 Runtime** | Microsoft's installed Evergreen browser runtime. This is distinct from the OSF UI runtime. |

The production render path is fixed: the OSF UI runtime owns one
`WebView2HostWebRenderer` and one `D3D12Compositor`; the browser stack itself
runs in the browser host. `IWebRenderer` and `ICompositor` are internal
abstraction seams, not player-selectable render backends.

## Versions

Always qualify a version by its domain:

| Term | Example/current field | Meaning |
|---|---|---|
| **OSF UI release version** | `version`, `targetVersion` | The installed OSF UI product release. A manifest or settings schema's `targetVersion` names this domain. |
| **web bridge protocol version** | `bridgeVersion` | The JSON protocol between one browser document and the OSF UI runtime. |
| **native ABI version** | `GetInterfaceVersion()` | The C ABI exposed through `OSFUI_RequestBridge`. |
| **browser-host IPC protocol version** | the shared `Wv2Protocol` constant | The private pipe protocol between `OSFUI.dll` and `osfui_webview2_host.exe`. |
| **WebView2 Runtime version** | browser-host diagnostics | Microsoft's installed browser-runtime version. |
| **data format version** | `manifestVersion`, schema `version`, `$schemaVersion`, `$formatVersion`, `configVersion` | A version local to one stored or authored data format. |

Do not use an unqualified *host version* or *runtime version*. In the bridge
handshake, `version` is the OSF UI release version and `bridgeVersion` is the
web bridge protocol version.

## View identity and UI units

- **Mod id (`modId`)** — the owning namespace, such as `acme.shiptools`.
- **View name (`viewName`)** — the name local to that mod, such as `dashboard`.
- **Qualified view id (`qualifiedViewId`)** — `<modId>/<viewName>`, such as
  `acme.shiptools/dashboard`. Public APIs that accept a view `id` mean this.
- **View** — the stable, manifest-addressed authored unit. Its identity comes
  from `views/<modId>/<viewName>/`; `manifest.json` declares no `id`. A legacy
  manifest `id` is ignored.
- **Document instance** — one Chromium load of a view. F5, hot reload, recovery,
  or reopening a reclaimed view creates a new document instance without
  creating a new view.
- **View kind** — `menu` or `hud`. A menu occupies the single active-menu slot;
  opening another menu replaces it. Multiple HUD views may remain open and are
  ordered by `order` beneath the active menu.
- **Overlay** — the combined OSF UI layer presented over the game, not a synonym
  for one view.
- **Render target** or **output surface** — a graphics resource. Reserve
  *surface* for graphics/API prose where its meaning is explicit; do not use it
  as the default noun for a view.

The generator's `--surface` flag is a frozen CLI spelling. In prose, its
`menu`, `hud`, and `settings` choices are **starter types**; the settings-only
starter does not create a view.

### Built-in UI names

- **Mod Settings** is the built-in `osfui/settings` view. Do not call the
  current UI the *Mods surface*, *settings hub*, or *MCM*.
- **Keybindings** is the built-in `osfui/keybinds` view. Do not call the current
  UI the *Keybinds board* or *input map*.
- **System Health** is a fixed destination inside Mod Settings, not a separate
  view and not a pinned-residency claim.

## View lifecycle and presentation

These states describe different axes and must not be used interchangeably:

| Term | Meaning |
|---|---|
| **discovered** | A valid manifest and qualified view id are known. No browser object is implied. |
| **instantiated** | The web renderer/browser host has created the live browser object for the view. |
| **load complete** | The document's main-frame navigation completed. This is the public `loadState: "loaded"` and the gate for completing a pending first open. |
| **open** | Presentation policy says the menu or HUD should be present. This is independent of main-frame load progress. |
| **active menu** | The one open menu selected for menu presentation and focus policy. There is no menu stack. |
| **input-target view** | The instantiated browser view selected by the renderer/browser host for mouse, real focus, cursor, and synthetic-key delivery. It follows the active menu during an input session, but names a transport target rather than presentation state. |
| **captures input** | The active menu's effective policy routes input to its document. This is not synonymous with menu kind or with `interactive`. |
| **suspended** | The document instance is retained, but WebView2 has paused its activity while hidden. |
| **reclaimed** | The document instance was destroyed and the stable view returned to the discovered state. |
| **pinned** | A residency policy: the view's live instance is not reclaimed. |
| **prewarmed** | The renderer primed a hidden first paint. Prewarming alone does not promise residency. |

Reserve **pinned** for view residency. A rail item that always stays in place,
such as System Health, is a **fixed destination**; it says nothing about the
browser lifetime of the containing view.

The public `osfui/views` payload retains compatibility field names:

- `loadState` reports browser main-frame progress (`unloaded`, `loading`,
  `loaded`, or `failed`);
- `open` reports presentation policy;
- `focused` identifies the active menu;
- `interactive` is a menu-kind capability summary, not current input capture;
- `hub` means catalog-visible; and
- `pinned` means not eligible for reclaim.

### Catalog and startup policy

Use **catalog visibility** in prose. The manifest/state compatibility field is
still named `hub`: `hub:false` means a hidden utility view that must not be
listed and is not eligible for discovered HUD auto-start.

`openOnStart` has two deliberately different entry-path effects:

| Entry path | Effect |
|---|---|
| normal discovery, HUD | Author default for the player's persisted **Start automatically** policy. |
| normal discovery, menu | Ignored; discovered menus never auto-start. |
| native plugin `RegisterView` | Explicit plugin opt-in: instantiate and open the registered view immediately, including a menu. |

When discussing author intent, call the HUD behavior the **auto-start default**;
when discussing `RegisterView`, call it **open on registration**.

## Readiness

*Ready* is never sufficient by itself. Use the qualified milestone:

- **browser-host ready** — the browser host has created its first controller
  and capture path and completed the private IPC startup milestone;
- **bridge available** — a document has the injected bridge capability;
- **bridge handshake ready** — the OSF UI runtime answered `osfui.hello` with
  the web protocol's `kind:"ready"` envelope;
- **preview initialized** — the CLI development preview settled its mock and
  page setup and can accept shell controls; it does not imply a bridge handshake;
- **frame ready** — a texture/fence is ready for the compositor.

The wire value `kind:"ready"` remains the bridge-handshake response and is not
the browser's main-frame load milestone.

## Bridge and retained state

- An **endpoint name** routes one web-to-native operation. A **send endpoint**
  is one-way; a **request endpoint** settles once with a reply or error.
- A **state key** names a latest-wins value; an **event name** names a one-shot
  happening. Do not call either a command.
- A **settings action** is a UI row that invokes a request endpoint. A
  **game input action** is a Starfield ControlMap action; those are unrelated.
- **Retained mod state** is state stored per publishing mod and replayed to
  each current or future document of that mod. Public compatibility methods
  remain named `SetViewState`/`SetView*`; their storage scope is not per view.
- **Platform** is an adjective for endpoints and state keys owned by OSF UI,
  normally in the `osfui` namespace. It is not another process or backend.

## Input

| Term | Meaning |
|---|---|
| **game input action** | A Starfield ControlMap event/action row. |
| **game binding** | One slot that maps a game input action to a physical input. A **game-binding collision** is a conflict or intentional share involving that slot. |
| **mod-hotkey binding** | The physical input assigned to a mod hotkey. |
| **mod hotkey** | A `type:"key"` settings value that triggers mod behavior. |
| **physical key name** | The layout-stable stored identity such as `Semicolon`. |
| **keycap label** | Localized display text such as `Ö`; never persist it as the binding. |
| **engine input context** | A live Starfield ControlMap context from `osfui/input-context`. |
| **hotkey context** | An authored `inputContexts` entry in a settings schema, local to one mod. |

## Health and diagnostics

- The **health registry** contains durable, actionable **health issues**.
- **System Health** is the built-in player-facing destination that renders the
  registry.
- **System information** is the factual environment summary attached to that
  registry.
- A **diagnostic report** is the local text export copied by the player. It is
  not the registry itself and OSF UI does not upload it.
- Logs remain chronological event streams. Do not use the health registry as a
  log or toast channel.

The public state key remains `osfui/diagnostics`, and public types retain names
such as `DiagnosticIssue`. An issue's `source` means its producer identity: a
platform subsystem for platform issues or an OSF UI runtime-assigned mod id for
plugin issues. Qualify which one is expected rather than treating `source` as a
type tag.

## Developer mode

**Developer mode** is the effective capability: verbose logging, hot reload,
and authoring DevTools. It can be enabled persistently by `devMode` in
`config.json`, or temporarily by the expiring **author-mode marker** written by
the view toolchain. *Author mode* names that temporary activation mechanism,
not a second permission level.
