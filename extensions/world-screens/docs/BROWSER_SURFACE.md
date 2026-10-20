# Browser surface — design

Status: 2026-10-20. Steps 1–3 (ABI 1.1, fullscreen presentation, pointer routing) are
implemented and covered by native tests, with in-game verification still pending.
Steps 4–7 are design only. Applies to the unreleased Surfaces ABI and World Screens 0.1.0.

## Goal

Make a world screen act as a TV with a real web browser. A player activates the screen,
which opens the screen's browser fullscreen with a mouse cursor, browser controls and an
address bar. They pick YouTube, a music stream or any other site. When they close it, the
same page keeps playing on the screen in the world.

This doc covers the core and extension work, and ends with a Papyrus-only TV mod
that is the landmark consumer.

## Why the overlay can't do this today

1. **Surfaces and the overlay are separate browsers.** `SurfaceService::Start` creates a
   dedicated `BrowserHostClient` per surface view (`instanceName="surface-"+profile`,
   `src/Surfaces/SurfaceService.cpp`). Opening a normal overlay view starts a different
   browser, so playback can't continue across the handoff.
2. **Surface interaction has no pointer.** It is keyboard/controller only. Surfaces
   call `SetPointerInputEnabled(false)`, and every mouse path in `src/Input/RuntimeInput.cpp`
   returns early while `SurfaceService::InputRenderer()` is set. The hardware cursor is
   also disabled for surfaces (`OverlayInputHook.cpp`, `wantHwCursor`).
3. **Every host is offline at the process level.** The WebView2 host launches with a dead
   proxy and `--host-resolver-rules="MAP * ~NOTFOUND"` (`tools/webview2_host/HostApp.cpp`,
   `kCaptureBrowserArguments`). Both hosts also cancel every navigation and request
   outside `https://osfui.example/<mod>/<view>/`.
4. **An untrusted page that posts a message fails the whole host.** `WebMessageReceived`
   reports a non-trusted source through `ReportSecurityFailure`, which ends in
   `FailHost("browser-security")`. A remote page calling `chrome.webview.postMessage` would
   therefore kill the browser. That's correct for local views but a denial of service for
   remote content.

## Shape of the solution

Don't move the page. Draw the surface's own browser fullscreen, then shrink it back.

```
                         ┌───────────────── browser surface host (one process) ─────────────────┐
 world screen(s) ◄─copy─ │  output texture (fixed size, e.g. 1920×1080)                          │
                         │   ├─ chrome view   (trusted, local, bridge)   hidden while in-world   │
 fullscreen quad ◄─draw─ │   └─ content view  (untrusted, remote, no bridge)                     │
                         └───────────────────────────────────────────────────────────────────────┘
```

- **One browser surface view, shipped by World Screens.** It's a reserved surface view
  (proposed id `osfui.browser/tv`) whose manifest is shipped by the extension. Content mods
  bind screens to that id and never ship pages for it.
- **Two views in one host.** The *content view* loads remote sites top-level. Most large
  sites, YouTube included, refuse to be framed, so iframes are not an option. The
  *chrome view* is a local trusted page with back/forward/reload, an address bar
  and close. It is stacked above the content view and shown only in fullscreen.
- **Fullscreen interaction** is a new presentation mode of the existing surface
  interaction. Core enters menu mode (FocusMenu, cursor, pause), draws the surface's frames
  as a letterboxed fullscreen quad in the UI pass, and routes mouse, keyboard and pad input
  to that host.
- **On exit** core releases input, hides the chrome view and restores the content view to full
  bounds. The browser keeps running for the world screens.

### Alternatives rejected

| Alternative | Why not |
|---|---|
| Open a normal overlay view and hand off the URL plus playback time | Two browsers. Loses logins, players and in-page state. Visible restart. |
| Chrome as an overlay-host menu drawn over the surface quad | Needs two compositors drawn together, cross-process hit testing and keyboard focus arbitration between hosts. One host with two views avoids all three. |
| Remote content in an iframe inside a trusted page | `X-Frame-Options`/`frame-ancestors` block most real sites. It would also put remote frames under a document that holds the bridge. |
| Let any content mod declare a remote view | Spreads the relaxed sandbox across mods. One audited, core-owned policy is easier to reason about. |

## Surfaces ABI 1.1

`sdk/OSFUI_Surfaces.h`. `API::Supports` accepts a 1.0 requester from a 1.1 provider, so
1.1 is additive. Changing the existing `BeginInteraction` slot would crash 1.0 builds of
Starcade and SSSE. The new method is appended with its own name, because MSVC groups
overloaded virtuals together and that would shift the existing vtable slots.

```cpp
inline constexpr std::uint32_t kVersion = 0x00010001;

enum class Presentation : std::uint32_t { InWorld, Fullscreen };
struct InteractionOptions
{
    std::uint32_t size{ sizeof(InteractionOptions) };
    Presentation presentation{ Presentation::InWorld };
};

// Appended after EndInteraction. BeginInteraction(...) == InWorld.
virtual Interaction BeginInteractionWith(Handle, const InteractionOptions&,
                                         InteractionCallback, void*) noexcept = 0;
```

Callback phases stay `Started`/`Ended`/`Rejected`. New rejection reasons:
`fullscreen-unsupported` (the manifest did not opt in) and `fullscreen-unavailable` (no
overlay compositor). Step 5 adds `closed` for the chrome view's close button. The existing
reasons (`focus-or-menu`, `overlay-opened`, `game-load`, `surface-unavailable`, …) keep their
meaning.

World Screens 1.1 forwards the options through the same additive pattern:
`IScreens::BeginInteractionWith(Binding, const InteractionOptions&, Callback, void*)`, plus a
`Client::BeginInteraction` overload. Papyrus gets a fire-and-forget entry point so the TV mod
needs no DLL:

```papyrus
; Requests fullscreen interaction for a bound screen. False = not admitted now.
Bool Function OpenFullscreen(String handle) Global Native
```

Admission keeps the current rules in `SurfaceService::UpdateInteraction`: a loaded host, a
ready frame, neutral input for two ticks, no overlay menu open or pending, and the game
window active. Fullscreen additionally requires FocusMenu to be admitted, the same as an
overlay menu.

## Manifest

A surface manifest gains one field. Only the reserved browser view may set it:

```json
{
  "kind": "surface",
  "width": 1920, "height": 1080,
  "texture": "...",
  "browser": { "chrome": "chrome.html", "chromeHeight": 56 }
}
```

`ViewManifest::Parse` rejects `browser` for any view whose id isn't the reserved one.
`browser` implies `fullscreen: true`. A plain surface may also opt into fullscreen with
`"fullscreen": true`. That's useful for Starcade-style pages that want a mouse, and it
gets no remote content.

**Threat model.** Installed mods already run native code, so this restriction doesn't
stop a malicious mod. Its purpose is to stop accidental exposure: only code OSF UI ships
decides what remote content can reach, and content-mod pages never share a host with remote sites.

## Resize

### v1: fixed output size

The browser surface renders at its manifest size (proposed 1920×1080) all the time.

- **Fullscreen** scales that texture to the backbuffer, letterboxed to preserve aspect
  ratio. There is no host resize, so `CopyFrame`'s matching-size contract is never
  violated. The 256 MiB World Screens output budget easily covers one 1080p target
  (≈8 MiB).
- **Content view bounds** change only for the chrome band:
  - Fullscreen: chrome at `(0, 0, W, chromeHeight)`, content at
    `(0, chromeHeight, W, H - chromeHeight)`.
  - In-world: content fills `(0, 0, W, H)` and chrome is hidden.

  Each transition is one page reflow. Video elements keep playing through a reflow.
- **CSS scale.** Use `SetViewport` (the CSS viewport, as distinct from `Resize`, which sets the
  pixel size) so text on a world screen is legible. For example, render 1920×1080 pixels with a
  1280×720 CSS viewport.

### v2 (optional): native-resolution fullscreen

`Resize` the host to the backbuffer on entry and back to the manifest size on exit.
Requirements:

1. `SurfaceFrames::Copy` must tolerate a source/destination size mismatch by reporting
   "not ready" and keeping the last good world texture. It must not fail the binding.
2. On exit, core restores the manifest size and only ends the interaction once a frame at the
   restored size has completed. Otherwise world screens show a stretched frame.
3. Repeat the backbuffer-resize handling that the overlay already does
   (`OnOutputResized`) while fullscreen.

Defer v2 until v1 has shipped. Softness at 1440p/4K is the only reason to do it.

## Drawing

`RecordOverlayIntoRenderTarget` (`src/Composite/D3D12Compositor.cpp`) draws the overlay
compositor's frames into the UI pass target chosen by `UiPass`. Fullscreen adds a
second source:

- `SurfaceService` exposes the active fullscreen surface's `SharedFrameConsumer`.
- The compositor draws that consumer into a letterboxed destination rect with black
  bars, instead of the overlay frames. Overlay and fullscreen surface are mutually
  exclusive: `PrepareViewOpen` already ends surface interaction with `overlay-opened`.
- **Readers.** World copies and the fullscreen draw read the same ring at the same time. World
  copies are **not** suspended. `SharedFrameState` tracks each read per command list with its
  own completion key. The ring is only replaced when no read is outstanding, and `Idle()` (which
  gates browser retirement) counts every reader. The one real hazard was the completion keys:
  both readers used the engine queue pointer as the key while signalling different fences. So
  `SurfaceFrames` now keys its copies by its own fence, and the compositor keys its draws by
  queue.
- **Implementation.**
  - `SurfaceFrames::RecordDraw` reserves the current frame for the engine's command list.
  - The compositor's queue hook reports that list's submission and completion to the surface
    consumer.
  - The surface's ring slots get their own SRV range after the overlay's.
  - The target is cleared to opaque black every frame while fullscreen, so the world never shows
    through before the first frame.

## Input routing

When a fullscreen interaction starts:

| Input | Route |
|---|---|
| Mouse move/button/wheel | Map game-window client coordinates to the letterbox rect, then to surface pixels. Inject into the surface host with `InjectMouseMove/Button/PhysicalMouseWheel` after `SetPointerInputEnabled(true)`. Ignore points outside the rect, but always forward button releases, as the overlay already does. |
| Hardware cursor | Allow it (`wantHwCursor` no longer excludes fullscreen surfaces). Hook `SetCursorChangeHandler` on the surface host so link and text cursors show. |
| Keyboard / text | Unchanged: `InputRenderer()` already returns the surface host. |
| Controller | `ui.gamepad` goes to the chrome view. Stick-as-cursor is a later addition; v1 is mouse-first. |
| Esc / pad Back | End the interaction (implemented). Step 5 adds a first stage for the content view: exit page fullscreen if it has a fullscreen element. |
| F12 (dev mode) | DevTools for the content view. |

Inside the host, pointer events go to the view whose bounds contain the pointer. A press
captures that view until release. Keyboard focus follows the last click, and Ctrl+L / F6
focuses the address bar. The host owns this routing because both views live in it. Core
treats the host as one input target.

The pause and cursor follow the menu path: while a fullscreen interaction is active, the
values passed to `SimPause::Apply` and `FreeCursor::Apply` in
`Runtime::ReconcileFrameState` must be true. Audio from the page keeps playing while the game
is paused. That's intended.

## Content view (untrusted)

The content view runs in a host launched in **remote mode** (a new `Init` field). It
inherits the existing per-view protections, plus:

### Process

- **Browser arguments.** Omit the dead proxy and the `MAP * ~NOTFOUND` resolver rules. Add
  `--autoplay-policy=no-user-gesture-required` so playback resumes after the browser is
  recreated. Keep the occlusion and backgrounding flags.
- **Profile.** Use a dedicated profile: `LocalOsfuiDir()/browser/` for WebView2 and
  `root_cache_path` for CEF. It stays outside the game's `Data` tree and MO2's VFS, so
  cookies never land in MO2's overwrite folder. "Clear browsing data" lives in the chrome
  view. WebView2: `ClearBrowsingData`. CEF: delete the profile on the next host start.

### Rules

| Concern | WebView2 | CEF |
|---|---|---|
| Bridge | Do not install `BridgeShim` or `NetworkGuard` in the content view. `put_IsWebMessageEnabled(FALSE)`. Messages from that view are dropped without `FailHost`. | No `osfui` V8 binding (renderer `OnContextCreated` already requires a trusted URL and the main frame). `OnProcessMessageReceived` drops untrusted sources. |
| Virtual host | No `SetVirtualHostNameToFolderMapping` on the content view. `https://osfui.example/*` is blocked for it. | The local-asset handler only answers trusted views. Block `osfui.example` in the content browser. |
| Navigation (top and frames) | `http`/`https` only. Block `file:`, `edge:`, `chrome:`, `view-source:`, `osfui.example` and private targets (below). Check every navigation, redirects included. | `OnBeforeBrowse`, same policy. |
| Private network | Block `localhost`, loopback, RFC 1918, link-local, `*.local` and IP-literal equivalents in `NavigationStarting`/`WebResourceRequested`. DNS rebinding is not covered; rely on Chromium Private Network Access for it. | `OnBeforeResourceLoad`, same list. |
| Popups | `NewWindowRequested`: if allowed, navigate the content view to the target and mark the event handled. | `OnBeforePopup`: same, return `true`. |
| Downloads | `DownloadStarting` → cancel. | `CanDownload` → `false` (already). |
| External schemes | `LaunchingExternalUriScheme` → cancel (`steam://`, `ms-settings:`, `mailto:`, …). | `OnProtocolExecution` → `allow_os_execution=false`. |
| Permissions | `PermissionRequested` → deny every kind (camera, mic, location, notifications, clipboard-read, MIDI, sensors, …). | `CefPermissionHandler`: deny media access and all prompts. |
| Dialogs | `AreDefaultScriptDialogsEnabled(FALSE)` + `ScriptDialogOpening` auto-dismiss. `BasicAuthenticationRequested` → cancel. `ClientCertificateRequested` → cancel. Certificate errors are never bypassed. | `OnJSDialog` suppress (already). `GetAuthCredentials` → `false`. `OnCertificateError` → `false`. |
| File pickers | No WebView2 API intercepts `<input type=file>`. The dialog would open behind the game. **Known gap**; it's a UX problem, not a security one, because the user picks the file. Test, and consider an injected script that disables file inputs in the content view. | `OnFileDialog` → cancel (already). |
| Autofill | `IsPasswordAutosaveEnabled(FALSE)`, `IsGeneralAutofillEnabled(FALSE)`. | Disable password manager prefs. |
| Browser UI | Context menus off (already). `AreBrowserAcceleratorKeysEnabled(FALSE)`, with the chrome view handling navigation keys. Check `window.print()`; it may open a print preview with no interception API. | Context menu cleared (already). |
| DevTools | Developer mode only (already). | Same. |

`FormControls` (the `<select>`/picker shim) may also be injected into the content
view, because native popups don't render into the captured visual. It is a UI
polyfill with no privileges. CEF draws popups through `OnPopupShow`.

## Chrome view (trusted)

A local page shipped by World Screens at `views/osfui.browser/tv/chrome.html`. It is a
normal trusted view: bridge, local-only requests, the existing guards. It lives in the
remote-mode host but keeps its own per-view guards, which are the only defence for local
views anyway. The process-level offline flags are defence-in-depth that this host gives up.

### Requests (page → core)

These go through a fixed `browser.*` set, which core validates:

| Request | Effect |
|---|---|
| `browser.navigate { input }` | Core classifies the input. A URL-like string is normalised to `https://` (`http` only if the user typed it). Anything else becomes a search URL from the configured engine. Core applies the content policy before navigating. |
| `browser.back` / `forward` / `reload` / `stop` / `home` | Delegated to the content view. |
| `browser.mute { muted }` | WebView2 `put_IsMuted`, CEF `SetAudioMuted`. Neither has a volume API. |
| `browser.clearData` | Clears the profile (see above). |
| `browser.close` | Ends the interaction with reason `closed`. |

### Events (core → page)

`browser.state` carries `url`, `title`, `canGoBack`, `canGoForward`, `loading`,
`secure`, `audible` and `muted`. The values come from host events (`SourceChanged`,
`DocumentTitleChanged`, `NavigationStarting/Completed`, `IsDocumentPlayingAudioChanged`),
never from the content page. The address bar shows the real origin with a lock or warning
state. That's the only anti-phishing measure, which is why it must not be spoofable.

### Persistence

The chrome page owns the home page, last URL and search-engine choice in its own
`localStorage` (local origin, so the content view can't reach it). On load it asks core to
navigate to the last URL, so the TV resumes where it left off after the browser retires.

## Consent

OSF UI's own OSF Settings schema gains **"Allow web browsing on World Screens"**,
default **off**. While it's off, core never launches a remote-mode host. The browser
surface then loads only the chrome view in a "browsing is disabled — enable it in
Mods → OSF UI" state. The setting's description says the screen loads real websites with
their own cookies and sign-ins, stored in the OSF UI profile folder.

## Lifetime and limits

- **Retirement.** The browser retires when the last binding releases demand (cell or 3D
  unload), so playback stops when the player leaves the area. The last URL comes back on the next
  bind (see Persistence), but playback position does not.
- **Mirroring.** Every screen bound to `osfui.browser/tv` shows the same browser. Separate
  channels would need extra reserved ids (`tv2`, `tv3`) with separate profiles; not v1.
- **Audio is not spatial or distance-attenuated.** Sound comes from the browser's own audio
  session. v1 has a mute button. Distance-based muting from the extension tick is a v2 option.
  True volume control needs per-process audio-session research (WebView2 audio plays from
  the Edge utility process, not the host).
- **Codecs and DRM.**
  - **Codecs:** YouTube and most streams serve VP9/AV1 + Opus, which both editions play.
    H.264-only sites may fail on CEF builds without proprietary codecs.
  - **DRM:** Widevine sites (Netflix, Spotify web player, Disney+) are unsupported in v1.
    Test WebView2 but promise nothing.
- **Sign-in.** Allowed. Cookies persist in the profile, password autosave is off, and the origin
  is always visible in the chrome view.

## TV mod (landmark consumer)

A Papyrus-and-content mod with no DLL:

- **Assets.** A craftable TV ACTI using DisplayScreen5. Reuse Starcade's craftable-TV
  pipeline and `tools/world_content.py`. Optionally, a few placed TVs in the Lodge and player homes.
- **Catalog.** `SFSE/Plugins/OSF/WorldScreens/screens/<mod>.json` registers the ACTI base.
- **Script** on the ACTI:

  ```papyrus
  String handle
  Event OnLoad()
      handle = OSFUIWorldScreens.BindScreen(Self, "osfui.browser/tv")
  EndEvent
  Event OnUnload()
      OSFUIWorldScreens.UnbindScreen(handle)
      handle = ""
  EndEvent
  Event OnActivate(ObjectReference akActionRef)
      If akActionRef == Game.GetPlayer() && handle != ""
          OSFUIWorldScreens.OpenFullscreen(handle)
      EndIf
  EndEvent
  ```

  Rebind after a game load and after workshop moves (both are covered by `OnLoad` and the existing
  load-rebind guidance). Activation text: "Watch".
- **Idle state.** Before the player has browsed, the content view stays on
  `about:blank` and the chrome view expands to full bounds as a "channel guide" landing page. The
  content view never loads `osfui.example`, so the landing page belongs to the chrome view. A
  fresh TV is never blank and never goes online without consent.

## Work breakdown

1. **ABI and admission** (done). `InteractionOptions`/`Presentation` in the Surfaces and World
   Screens headers, additive 1.1 entry points, and the manifest `fullscreen` opt-in.
   `SurfaceService` requests carry the presentation. Papyrus `OpenFullscreen`. Existing 1.0
   consumers need no change.
2. **Fullscreen presentation** (done). Menu-mode pause and free cursor, and a compositor
   source switch with a letterbox rect over black. Concurrent reads replace the planned
   world-copy suspension (see Drawing).
3. **Pointer routing** (done). Letterbox mapping shared with drawing (`FitCentered`), bar
   presses swallowed with releases always forwarded, wheel and pad wheel, and the hardware
   cursor with the page's cursor shapes.
4. **Host remote mode.** `Init` flag, process args, profile and the content-view policy
   table in WebView2, then CEF. Drop untrusted web messages without `FailHost` in remote mode.
5. **Two-view browser surface.** Content and chrome views in one surface host, bounds per
   presentation, in-host hit testing and focus, and the `browser.*` requests and events.
6. **Consent setting** and the disabled state.
7. **TV mod.**

Steps 1–3 also give plain surfaces (e.g. Starcade) an optional mouse-driven fullscreen
mode, and can ship before any remote content exists.

## Acceptance (in-game, both editions)

- Activate, browse, close: playback continues on the world screen without a restart.
- Esc exits page fullscreen first, then the interaction. Pad Back closes it.
- Focus loss, opening an overlay menu, loading a save and cell unload each end the
  interaction cleanly and restore cursor, pause and input.
- Blocked: downloads, `steam://` links, popups opening windows, permission prompts,
  `http://localhost` and router addresses, and `https://osfui.example/...` typed in the
  address bar.
- `chrome.webview.postMessage` from a remote page does not fail the host.
- Consent off: no remote-mode host process is launched.

## Open decisions

1. Reserved id: `osfui.browser/tv`, or a World Screens-owned mod id.
2. Default search engine and home page.
3. Fullscreen always pauses (implemented). Decide whether to expose that as an option.
4. Plain surfaces can already set `fullscreen: true` (steps 1–3 landed). Decide whether to
   ship that before the browser.
