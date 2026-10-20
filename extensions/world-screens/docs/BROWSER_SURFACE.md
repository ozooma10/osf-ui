# Browser surface — design

Status: 2026-10-20. Steps 1–3 (ABI 1.1, fullscreen presentation, pointer routing) are
implemented and covered by native tests. The WebView2 fullscreen draw, letterbox,
clicks/cursors and audio continuing after Esc passed an in-game check on 2026-10-20;
CEF Watch/close and WebM playback also passed the private audio-prototype run below;
the remaining acceptance cases are still pending.
Steps 4–6 are implemented for WebView2 and CEF. WebView2 public video playback and
Watch/close passed a basic in-game check; broader browser-runtime acceptance is pending.
Steps 4–5 are committed as `c06f8f14`; step 6 as `cddb291a`. Browsing defaults off.
Remote mode can launch only when the OSF Settings consent toggle was enabled at game
startup. Step 7 is implemented as the separate Papyrus-only OSF Browser TV content mod;
Watch opens its browser and playback continues after Esc on WebView2. CEF camera
audio, multiple TVs, save/load rebinding and cell-unload retirement have passed
direct-command acceptance; crafting and the other browser cases remain pending.
Applies to the unreleased Surfaces ABI and World Screens 0.1.0.

CEF spatial audio is implemented, tested and deployed in the ordinary host
(Surfaces ABI 1.2 / private browser protocol 8). Build/native verification and
fresh game acceptance are recorded below; the earlier compile-time prototype is
historical. WebView2 spatial audio is also implemented in the ordinary host using
an embedded tab-capture extension and Web Audio. Its current game-host run on
runtime 155.0.4283.62 records 211 passing checks, zero failures and two conditional
geometry skips, including spatial tones and public video playback. Final-payload
collector recovery, save/load and cell retirement passed; cleanup and matching
normal-mod deployment are verified. Remaining limits and exact evidence are in
the [WebView2 validation record](../../../tools/profiling/webview2-game-audio/VALIDATION.md).

## Goal

Make a world screen act as a TV with a real web browser. A player activates the screen,
which opens the screen's browser fullscreen with a mouse cursor, browser controls and an
address bar. They pick YouTube, a music stream or any other site. When they close it, the
same page keeps playing on the screen in the world.

This doc covers the core and extension work, and ends with a Papyrus-only TV mod
that is the landmark consumer.

## Original constraints

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
  (id `osfui.browser/tv`) whose manifest is shipped by the extension. Content mods
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
  "manifestVersion": 1,
  "kind": "world",
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

The browser surface renders at its manifest size (1920×1080) all the time.

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
| Esc / pad Back / pad B | Exit page fullscreen first; otherwise end the interaction. Plain surfaces retain their existing immediate exit. |
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
| `browser.guide { open }` | While browsing fullscreen, shows chrome's full-frame guide over the page (content hidden, page keeps loaded and playing). Back, any navigation and the end of the interaction close it. Rejected with `browser-not-interactive` outside fullscreen. |
| `browser.getState` | Returns the current native state, including enabled/landing/fullscreen flags and any site-load error. |

Only the reserved chrome view may invoke these requests. Remote page messages are
dropped at both host and core boundaries. Navigations accept an optional
`searchEngine` key (`duckduckgo`, `google`, `bing`); arbitrary search URL templates
are rejected. `browser.home` with an empty input turns the channel off: content goes to
`about:blank` and every screen shows the local guide.
Native commands complete deferred requests by token; failure/retirement rejects
outstanding requests and late results are ignored.

### Events (core → page)

`browser.state` carries `url`, `title`, `canGoBack`, `canGoForward`, `loading`,
`secure`, `audible` and `muted`. The values come from host events (`SourceChanged`,
`DocumentTitleChanged`, `NavigationStarting/Completed`, `IsDocumentPlayingAudioChanged`),
never from the content page. The address bar shows the real origin with a lock or warning
state. That's the only anti-phishing measure, which is why it must not be spoofable.

The event also carries `audibleKnown`, `enabled`, `fullscreen`, `landing`, `guide`,
`screens` (bound screens sharing this browser) and `error`. `landing` is true whenever
chrome has the full frame: no page, a site error, browsing disabled, or the guide open.
The pinned CEF API supplies mute state but no audibility observation without installing
an audio-capture handler (which changes playback ownership). CEF reports
`audibleKnown: false`; chrome omits the playing-audio indicator for that edition.
`browser.focusAddress` is a host-owned event for Ctrl+L/F6.

### Persistence

The chrome page owns the home page, last URL and search-engine choice in its own
`localStorage` (local origin, so the content view can't reach it). On load it asks core to
navigate to the last URL, so the TV resumes where it left off after the browser retires.
The agreed defaults are the local landing page and DuckDuckGo. Chrome can save the
current page as Home and cycle through the three search engines. Clear data resets
these preferences. WebView2 clears browsing data through `ClearBrowsingDataAll`;
CEF writes a marker and removes this surface's `Default` and `Content` caches before
the next `CefInitialize`, reporting `restartRequired: true` to chrome.

## Consent

OSF UI's own OSF Settings schema has **"Allow web browsing on World Screens"**
(`osfui/allowWebBrowsing`), default **off**, in the **World Screens** group.
It is a **restart setting**: both enabling and disabling take effect on the next
Starfield launch. `OSFSettingsClient` reads it once during startup, using the same
pattern as developer mode. A missing, wrong-type, unavailable or failed read gives
no consent and reports the existing startup-setting warning in Mod Issues.

With effective consent off, core never launches a remote-mode host. The browser
surface then loads only the chrome view in a "browsing is disabled — enable it in
Mods → OSF UI, then restart Starfield" state. The setting's description says the screen
loads real websites with their own cookies and sign-ins, stored in the OSF UI profile
folder. Enabling the setting does not create a host by itself: the reserved view must
have surface demand. Plain surfaces and overlay views stay offline regardless of consent.

The runtime passes the startup value to `SurfaceService`'s browser-creation callback.
Changing the saved setting mid-session does not change a running host's security mode,
nor the mode used by later surface bindings or host recovery in that session. Restarting
is required to stop existing web playback and enforce a newly disabled value.

## Lifetime and limits

- **Retirement.** The browser retires when the last binding releases demand (cell or 3D
  unload), so playback stops when the player leaves the area. The last URL comes back on the next
  bind (see Persistence), but playback position does not.
- **Mirroring.** Every screen bound to `osfui.browser/tv` shows the same browser. Separate
  channels would need extra reserved ids (`tv2`, `tv3`) with separate profiles; not v1.
- **Audio is not spatial or distance-attenuated.** Sound comes from the browser's own audio
  session. v1 has a mute button. Distance-based muting from the extension tick is a v2 option.
  The 2026-10-20 WebView2 prototype captured PCM from the Edge audio-service process,
  but muting its Windows session also silenced the captured PCM. This combination
  cannot feed a positional player while suppressing the original output. See
  [capture/suppression results](../../../tools/profiling/browser-audio/README.md).
  The private CEF PCM/XAudio2 prototype now proves positional playback and output
  replacement without duplicate browser sound; it is not enabled in shipping
  targets. See the [CEF prototype](../../../tools/profiling/cef-audio/README.md).
  WebView2 still needs a different capture/output path.
- **Codecs and DRM.**
  - **Codecs:** YouTube and most streams serve VP9/AV1 + Opus, which both editions play.
    H.264-only sites may fail on CEF builds without proprietary codecs.
  - **DRM:** Widevine sites (Netflix, Spotify web player, Disney+) are unsupported in v1.
    Test WebView2 but promise nothing.
- **Sign-in.** Allowed. Cookies persist in the profile, password autosave is off, and the origin
  is always visible in the chrome view.

## TV mod (landmark consumer)

[OSF Browser TV](../../browser-tv/README.md) is a separate Papyrus-and-content mod
with no DLL, implemented under `extensions/browser-tv`.

- **Plugin.** `OSFBrowserTV.esm` is a full ESM with only `Starfield.esm` as a master.
  Local `800` is the DisplayScreen5 ACTI, `801` its Decorations recipe, and `802`
  the "Watch" activation text message. It adds no placed references or vanilla overrides.
- **Assets and crafting.** The builder uses `tools/world_content.py` and the same
  vanilla ACTI, wall mounting data and recipe sources as Starcade's craftable TV.
  It creates **Browser TV** under Decorations, costing 4 Aluminum, 2 Zero Wire and
  2 Beryllium. Vanilla models are referenced, not copied into the package.
- **Catalog.** `SFSE/Plugins/OSF/WorldScreens/screens/osf.browser-tv.json` registers
  local ACTI `800` with profile `display-screen5`.
- **Script.** `OSFBrowserTV.psc` binds `osfui.browser/tv` on initialization, load/cell
  attach, player save load and workshop placement/movement. It retries initialization
  for up to ten seconds; activation can retry after that. Repeated binding is
  idempotent and does not retire an existing browser. Handles are reacquired after
  load, never trusted because a saved string is nonempty. Unload, cell detach and
  workshop removal cancel timers, unregister the player event and release the handle.
  Guards serialize binding changes and prevent overlapping activation requests.
- **Activation.** Player activation requests `OSFUIWorldScreens.OpenFullscreen`.
  An immediate failure displays a short "try again" notification; it does not
  schedule a later automatic open. The extension still owns asynchronous admission.
- **Build.** `pwsh tools/build-browser-tv.ps1` compiles Papyrus and generates the
  plugin/catalog in a fresh `build/browser-tv` staging directory. It validates the
  actual payload against the vanilla inputs. `-Deploy` copies its seven owned files
  into `XSE_SF_MODS_PATH/OSF Browser TV` and verifies hashes, without changing profiles.
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
4. **Host remote mode** (implemented; browser-runtime acceptance pending). Both hosts
   consume `Init.remoteMode` and `Navigate.remoteContent`/`url` (introduced in
   protocol 6; step 5 advances the matched core/hosts to protocol 7).
   The process mode defaults off; the view role is fixed before controller/browser
   creation and survives core's host-recovery replay. Remote views cannot become trusted
   local views by navigating, and their page messages are discarded by both host and core.
   `tools/browser_shared/BrowserRemoteUri.h` implements the shared absolute
   HTTP/HTTPS gate, with `remote_uri_tests` in the native runner and XMake.
   It rejects local names, the virtual asset host, private IP literals, IPv4 aliases,
   private IPv4 embedded in mapped/NAT64/6to4 IPv6, credentials and ambiguous authorities.
   IDNs must be normalized to ASCII/IDNA before this gate; raw Unicode hosts are rejected.
   Both hosts apply it to navigation and intercepted requests. Remote content receives
   neither the bridge/network shim nor a local asset mapping/handler. The idle document
   may be `about:blank`; child `about:srcdoc` and in-memory data/public-origin blob
   resources grant no local privileges.
   WebView2 installs the required deny handlers/settings before navigation and filters
   all request source kinds, including workers. CEF uses a dedicated request context
   for content, including requests without a browser (service workers), and checks
   redirect destinations in `OnResourceRedirect`. CEF cannot cancel in that callback,
   so it replaces a rejected redirect with `about:blank`.
   Profiles are under `LocalOsfuiDir()/browser/{webview2,cef}/`, followed by the existing
   instance-name directory. CEF keeps content's `Content` cache separate from trusted
   chrome's `Default` cache. Required settings/preferences fail closed.
   `BrowserHostConfig.remoteMode` and `CreateOrNavigateRemoteView` are internal plumbing.
   Step 6's startup consent is the only production path enabling the reserved browser.
5. **Two-view browser surface** (implemented; browser-runtime acceptance pending).
   World Screens ships `osfui.browser/tv`, its trusted chrome files, named placeholder,
   materials for both `display-screen5` and `full-uv`, and their support textures.
   The core creates a separate `osfui.browser/content` view only when the internal
   consent callback permits it; step 6 connects the production caller to Settings.
   Without effective consent the local disabled page is the only view. Both hosts implement per-view
   bounds, press capture, click focus, Ctrl+L/F6, content DevTools and fullscreen-first
   Back. Layout changes preserve both documents and output dimensions; the temporary
   frame-readiness gap during repaint no longer ends a browser interaction.
   `browser.*` commands and native state drive chrome. A site-load error expands the
   local page for retry instead of failing the surface host.
6. **Consent setting** (implemented; Settings/in-game acceptance pending).
   Default-off `allowWebBrowsing`, explicitly requiring restart, is read through
   `OSFSettingsClient` and wired into reserved browser host creation. Disabled chrome
   points to Mods → OSF UI and explains the restart. A failed read stays offline.
7. **TV mod** (implemented; in-game acceptance pending). Separate OSF Browser TV
   full ESM, Papyrus script, catalog and build/deploy helper. No new DLL, framework
   copies or placed-reference overrides. See the consumer and verification sections.

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

### Verification record — 2026-10-20

- **Build under test:** `1babb8b1` on `dev`, WebView2 edition, runtime
  `155.0.4283.62`, RTX 3080, 3440×1409 client area. Core and extension were built
  and deployed; their source/deployment hashes matched.
- **Fixture:** `tests/manual/OSFFullscreenProbe.psc` and `fullscreen.html`, deployed
  only in the temporary `OSF Browser Fullscreen 20261020` MO2 profile/mod. The
  `starcade.arcade/screen-play` override opts into fullscreen at 1200×600. Nothing
  from this fixture belongs in the extension package.
- **Initial failure:** invoking `OpenFullscreen` synchronously from the console
  produced `Rejected/unavailable` because the console blocks surface interaction.
  The corrected probe uses `Utility.Wait(1.0)` (which does not count menu time)
  before requesting admission. The old failure also occurred after the browser
  was loaded; it was not a cold-start/frame-readiness failure.
- **Direct commands:** the existing native SFSE mailbox dispatched
  `coc SettleRedMile02` and `cgf "OSFFullscreenProbe.OpenRedMile"` in an isolated
  copy of the pinned test save. Native cell readback confirmed the teleport.
  No Computer Use or injected input was used for this successful run.
- **Observed:** the compositor logged its first fullscreen surface draw,
  `1200x600 -> 3440x1409`, scale `2.348`. A game-window capture showed the page
  centered with opaque black side bars. The user confirmed all corner clicks,
  hand/text cursors, Esc closing and the tone continuing afterward. Post-Esc
  native readback reported no open menus and `pauseRequestCount=0`.
  Subsequent fullscreen attachments and detachment on focus loss were logged.
- **Evidence:** local captures and mailbox receipts are under
  `build/browser-fullscreen/evidence/20261020-045542-379-FullscreenNativeCommands/`.
  The first capture shows document `08abd3a4` at 7.2 seconds. The later capture
  occurred after focus loss had already closed fullscreen, so it does **not**
  establish the document ID/timer on reopen. The owned game exited through `qqq`.
- **Native verification:** all 32 suites passed, including 141 remote-URL checks
  and 58 `surface-fullscreen` checks. The CEF asset symlink-escape case was skipped
  because Windows denied symlink creation.
- **Still open:** same-document/timer comparison on reopen, direct visual
  confirmation of the continuing animation on the bound world screen, black-bar
  click rejection, pad Back, save/load and unload acceptance, and CEF in-game
  verification. Remote browsing had not been implemented at this checkpoint.

The URI gate deliberately rejects ambiguous inputs instead of reproducing the
browser's repair rules. Its numeric-host cases follow the
[WHATWG URL host parsing rules](https://url.spec.whatwg.org/#host-parsing);
address-range references are the IANA
[IPv4](https://www.iana.org/assignments/iana-ipv4-special-registry/) and
[IPv6](https://www.iana.org/assignments/iana-ipv6-special-registry/) registries.
The gate does not resolve DNS. DNS rebinding remains subject to Chromium's private-network
protections, as stated in the design; it is not prevented by this parser.

### Step 4 build/native verification — 2026-10-20

- WebView2 host, CEF host and core compile successfully against the local SDKs.
  The core build deployed the matched WebView2 edition through XMake. CEF was built
  into `build/cef-host/runtime`; the selected deployed edition was not changed.
- All 32 native suites passed. `remote_uri_tests` now covers 297 URL, role, resource,
  profile and process-argument checks; `wv2_messages_tests` passes 64 checks including
  default-off mode and remote navigation IPC. The CEF symlink case remains skipped
  because Windows denied symlink creation.
- These checks do **not** execute a remote browser. Actual public navigation/playback,
  mixed trusted/content views, redirects, worker requests, permission/download/dialog
  suppression, CEF preference acceptance, and persistence still need browser-runtime
  acceptance. No game or browser harness was launched for step 4.
- Native request callbacks are not a general network firewall. In particular,
  WebView2's documented `WebResourceRequested` schemes do not include WebSockets;
  non-HTTP transports and private-target coverage must be resolved/tested before
  enabling browsing for users. The design's WebView2 file-picker and print-preview
  gaps remain open. Remote content currently receives no optional form-control shim.

References for these runtime limits:
[WebView2 request interception](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2#add_webresourcerequested)
and [Chromium password preferences](https://github.com/chromium/chromium/blob/main/components/password_manager/core/common/password_manager_pref_names.h).

### Step 5 build/native verification — 2026-10-20

- Core, WebView2 host, CEF host and World Screens build successfully. Core/WebView2
  and the extension were deployed through XMake, including an explicit extension
  install. SHA-256 matches were verified for all three deployed binaries and all
  five browser manifest/chrome/texture assets. CEF is staged in `build/cef-host/runtime`;
  the deployed edition remains WebView2.
- All **33 native suites** pass. The new browser policy/geometry/capture suite has
  46 checks; enabled and default-disabled browser lifecycle scenarios have 71 and
  37 checks. Manifest validation passes 124 checks, shared IPC 70, and the remote
  URI policy 297. Existing fullscreen coverage remains green. The CEF asset
  symlink-escape case is still skipped because Windows denied symlink creation.
- Lifecycle coverage includes non-chrome request rejection, URL rejection before
  dispatch, deferred results, remote-message isolation, close/Back handling,
  unchanged navigation counts across presentation changes, temporary frame
  unreadiness during reflow, site errors and late results after retirement.
- Chrome JavaScript passes `node --check`. Build/test logs are
  `build/browser-surface-{core-build,cef-build,extension-build,native}.log`.
  No browser harness or game was launched for this step, and no commit was made.
- **Still unverified in game:** two-view composition and hit testing in both
  editions, Ctrl+L/F6, page-fullscreen Esc, content DevTools, actual website playback
  across close/reopen, preference/profile clearing and persistence. The step 4
  network/file-picker/print limitations above remain open. CEF audibility is
  explicitly unknown; native mute is implemented. Step 6's startup consent gates
  production remote host creation.

### Step 6 build/native verification — 2026-10-20

- All **33 native suites** pass. Three new runtime consent scenarios each pass
  39 checks through the production Settings callback: allowed, disabled, and failed
  read. They verify that consent never enables remote mode for plain world surfaces
  or the overlay, and that changing a saved restart setting cannot change the
  effective value during the session.
- Settings client tests cover the default-off schema, missing settings, wrong-type
  and unavailable/error reads, independence from developer mode, and the next-start
  behavior. Chrome JavaScript syntax also passes.
- Core and World Screens build successfully after Starfield exited. Core/WebView2
  and the extension were deployed through XMake, including an explicit extension
  install. SHA-256 matches were verified for the three deployed binaries, consent
  schema and browser chrome. The deployed edition remains WebView2.
- Evidence: `build/browser-consent-{core-build,extension-build,native}.log` and
  `build/browser-consent-deployment.json`. Settings UI/in-game acceptance remains
  pending; no game or browser harness was launched for this step.

### Step 7 build/native verification — 2026-10-20

- `OSFBrowserTV.psc` compiles with the CK Papyrus compiler. Five content checks
  pass against the generated full ESM and installed vanilla inputs: record/master
  scope, model/bounds/mounting, VMAD property resolution to the Watch message,
  recipe cost/category/result, and catalog/payload ownership.
- All **33 native suites** pass, including browser consent and fullscreen lifecycle
  coverage. The existing CEF symlink-escape case remains skipped because Windows
  denied symlink creation.
- Core and World Screens build successfully and are deployed through XMake,
  including the explicit extension install. The deployed edition remains WebView2.
- The TV's seven payload files are deployed into `MO2/mods/OSF Browser TV`.
  Their SHA-256 hashes and the three framework binary hashes match the build
  outputs. Mod/profile enablement was not changed.
- Evidence: `build/browser-tv-{build,core-build,extension-build,native}.log`;
  `build/browser-tv/latest-build.json` points to the content build and payload hashes;
  `build/browser-tv-framework-deployment.json` records the framework binary hashes.
- **Still unverified in game:** new TV discovery in both workshop menus, resource
  consumption, wall placement/movement/removal, Watch activation, save/load,
  mirroring and playback after closing. The remaining browser acceptance above
  also applies. No game or browser harness was launched for this build step;
  the following runtime record supersedes its Watch/playback gap.

### Browser TV runtime verification — 2026-10-20

- Enabled OSF Browser TV, World Screens and `OSFBrowserTV.esm` in MO2's Default
  profile. Runtime checks used a disposable profile/save, with startup browsing
  consent enabled only in private Settings values.
- Found and fixed a packaging omission: the reserved view had its feed DDS but
  lacked its generated screen materials and support textures. Binding failed
  before browser startup. World Screens now owns both material profiles and all
  referenced textures; its explicit install deploys them.
- The extension built and installed successfully. Five screen asset/world view
  tests passed, as did the staged World Screens package check. No framework C++
  changed in this repair.
- A private Papyrus setup command spawned the actual TV ACTI, then invoked
  `Activate(Game.GetPlayer())`. The production TV script handled activation.
  Native readback confirmed fullscreen interaction, `OSFUI_FocusMenu` and pause.
  The public Sintel trailer played on the bound TV; a game capture showed the
  **WATCH** activation prompt.
- Esc released the focus menu and pause while native audio remained active and
  the trusted chrome document survived. The user confirmed continued playback.
  A longer automated follow-up was interrupted by another activation, so it is
  not a passing five-second uninterrupted-close assertion.
- Evidence: `build/browser-tv-game/runs/20261020-142219-680-browser-tv/`.
  Workshop crafting/placement, save/load, multiple-TV behavior, page fullscreen,
  broader security acceptance and CEF runtime checks remain open.

### WebView2 audio capture/suppression prototype — 2026-10-20

- Built the standalone native probe in `tools/profiling/browser-audio` and ran it
  against a real Browser TV in a fresh disposable Starfield session through direct
  commands. No computer use, foreground transfer or injected input was used.
- Actual WebView2 audio-service capture delivered 48 kHz stereo PCM. A controlled
  997 Hz tone and an unmodified remote video both became silent in the capture
  when the owned browser's Windows audio session was muted. Output and capture
  recovered after restoration. The original session volume was unchanged.
- This is a **negative feasibility result** for process-loopback plus session mute,
  not a spatial-audio implementation. The prototype is outside shipping targets.
  It does not mute the whole device, install a virtual device or inject into Edge.
- Evidence, limits and reproduction are in the
  [prototype record](../../../tools/profiling/browser-audio/README.md).

### CEF raw audio / positional TV prototype — 2026-10-20

- Added a compile-time-only `CefAudioHandler` experiment for the reserved browser
  pair. It receives 48 kHz stereo PCM, downmixes to a bounded mono queue, and uses
  XAudio2/X3DAudio to play from the actual TV position relative to the player.
  The normal build remains unchanged in audio behavior; `-AudioProbe` stages a
  separate private runtime.
- Direct native game commands verified panning, player turning, distance falloff,
  recovery on returning, and capture while replacement output is disabled. The
  near/far tone difference was **16.9 dB** at about 2.5 m / 17.5 m. With replacement
  output disabled, raw PCM remained unchanged and no original browser tone leaked.
- An unmodified public WebM video played through the content callback, during
  fullscreen Watch, and at least five seconds after closing. The same content
  stream and trusted document survived. No computer use, focus transfer or input
  injection was used: `browser.close` exercised interaction teardown; this run did
  not inject a physical Esc key.
- The prototype retains its last pose while fullscreen pauses Papyrus and resumes
  live updates on close. The final run recorded no dropped callback packets.
  Disabling/re-enabling the real TV restarted its host; disabling it again retired
  the host in **10.625 seconds**, including the existing ten-second feed grace.
- Fixed a prerequisite CEF startup race: pipe processing/browser creation now waits
  for global context initialization and, for remote hosts, isolated-context policy
  readiness. Both ordinary and prototype CEF hosts build; all **33 native suites**
  pass (the existing symlink-permission skip remains).
- The private CEF core/runtime hashes match their staged files. The normal deployed
  WebView2 core, host and World Screens DLL hashes remain unchanged. The owned game
  exits normally and MO2 is restored to Default.
- This establishes feasibility, not shipping spatial audio. The prototype uses
  player head/yaw instead of the rendered camera, one selected TV, a simple rolloff,
  and an independent audio engine. Game sound settings, occlusion/room acoustics,
  device changes, multiple TVs and production pose/lifecycle integration remain.
- Results and reproduction:
  [CEF prototype record](../../../tools/profiling/cef-audio/README.md);
  `build/cef-audio-game/runs/20261020-155921-244-cef-audio/audio-results.json`.

### Production CEF spatial audio — 2026-10-20

- `SpatialAudio` is part of the ordinary CEF build. The two reserved browser views
  receive raw PCM and suppress Chromium's direct output. Other views retain their
  existing audio behavior. The later WebView2 implementation is described below.
  No page script supplies positions.
- World Screens captures the **main world scene camera**, including its full
  forward/up basis, and loaded TV scene transforms on the verified
  `NativeMainThreadQueue` drain. Both use render coordinates so floating-origin
  rebasing cancels. `PlayerCamera.cameraRoot` is a viewmodel camera and is not used.
  The scene-camera accessor has a 1.16.244.0 version/instruction gate; it does not
  call CommonLib's currently unbound `Main::WorldRoot` ID.
- Surfaces ABI **1.2** appends `PublishAudioScene` under a distinct virtual name;
  1.0 and 1.1 requesters still work. It copies bounded POD snapshots, forwards them
  only to a consent-enabled browser host, and retains the original sample deadline
  across IPC. Invalid snapshots, missing camera, loading/main menus and stalled
  updates fail silent. Empty scenes remove audio without waiting for GPU retirement.
- All TVs still mirror one browser page. Up to 32 nearest, bound, attached TVs
  within 20 m act as emitters. Disabled/unloaded references, deleted or remapped
  bindings, save replacement and unbinding remove their emitters. No engine
  reference or scene pointer is retained by the core or browser.
- Identical mono PCM uses the sum of the emitters' X3DAudio matrices, equivalent
  to synchronized copies of the same source. Normalization prevents a cluster
  from multiplying volume without limit. Rolloff is full through 1 m and zero at
  20 m. Motion is smoothed; mute, disappearance and stale snapshots silence
  immediately. Browser mute and the game's master-volume setting apply.
- The CEF callback copies into four fixed 10 ms blocks; four submitted buffers
  bound output latency. Overflow drops the oldest pending packet. Stream changes
  destroy the previous voice before reusing buffers. Audio devices initialize
  when the reserved views are created, before the first PCM packet. XAudio2's default virtual
  device follows endpoint changes, with a one-second rebuild retry after critical
  errors. Callback/storage destruction is ordered. Host shutdown silences first.
- Developer-mode `browser.getState().audio` exposes capture counters, errors and
  spatial snapshots for direct-command acceptance. Production writes no periodic
  audio evidence files and recognizes no prototype pose/output event.
- This is positional playback through XAudio2, with no Wwise room acoustics,
  occlusion or Doppler. It does not create independent browsing sessions per TV.
  Hardware endpoint removal/switching recovery is implemented but has not been
  exercised in game. The acceptance below covers the installed eight-channel
  endpoint and its Windows stereo downmix.

The output-device behavior follows
[XAudio2 critical-error recovery](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/nf-xaudio2-ixaudio2enginecallback-oncriticalerror).

### CEF production audio acceptance — 2026-10-20

- Implementation commits: `e495ce2e` (camera, binding lifecycle and multiple TVs)
  and `43ac0790` (prepare audio devices before first playback). CEF core/host,
  World Screens and WebView2 host builds pass. All **34 native suites** pass,
  including 47 audio validation/mixing/deadline checks and 82 enabled / 40 disabled
  browser lifecycle checks. The existing symlink-permission skip remains.
- The full direct-command run records **34 passing runtime checks** in
  `build/cef-spatial-game/runs/20261020-163213-597-cef-spatial/audio-results.json`.
  Free camera retained the listener position while the player moved 10 m.
  Panning followed the camera's actual orientation, including its orientation
  differing from the player's after leaving free camera.
- Two real TV references contributed separate positions to one browser stream.
  Moving a TV updated its emitter. Disabling one left the other playing; disabling
  both silenced output while raw PCM continued. Re-enabling before cache retirement
  restored both without restarting that stream. Out-of-range and browser-mute
  measurements were also silent, with no leaked original Chromium output.
- The measured distance attenuation was **16.89 dB** in the tested near/far
  positions. With free camera holding the listening geometry fixed, changing the
  private session's master volume from 0.7 to 0.35 produced **0.500017×** RMS;
  restoring 0.7 restored output. No steady-playback packet drops were observed.
- An unmodified public WebM video remained audible through Watch and at least
  five seconds after close, preserving stream generation and trusted document.
  Native camera updates stayed fresh while fullscreen paused the game. Commands
  used `browser.close`; no physical Esc key, computer use, focus transfer or input
  injection was used.
- Save/load renewed both bindings and positional audio worked after restarting
  the fixture tone in the recreated browser. The first test incorrectly expected
  that fixture tone to survive browser recreation; its failed observation is
  retained, followed by `save-load-audio-confirmed.json`. Cell detachment retired
  the old browser/audio host in **6.658 seconds**.
- The first run exposed 15–16 dropped packets during device activation.
  `43ac0790` prepares the devices before playback. A fresh run of that final build,
  `build/cef-spatial-game/runs/20261020-164854-106-cef-spatial/`, records **10 passing
  final checks**, zero dropped packets for both the first tone and remote content,
  and two-TV Watch/close continuity. Its native WGC capture shows both TVs mirroring
  the video; both owned games exited normally and MO2 was restored to Default.
- The normal MO2 core, extension, CEF host and chrome hashes match the tested
  staged payload. All **240 CEF runtime files** match the runtime manifest.
  Evidence: `build/cef-spatial-deployment.json`, the final run's `deployment.json`,
  `build/cef-spatial-{core-build,native,deploy}.log`. Browsing consent was changed
  only in private test values, and no test DLL or tone fixture was installed into
  the normal mod folders.

### Production WebView2 spatial audio — 2026-10-20

- The ordinary WebView2 host forwards the same Surfaces ABI 1.2 camera/emitter
  snapshots used by CEF. This route is limited to the two reserved browser TV views
  in the consent-enabled remote host. Ordinary local overlay and surface views keep
  their existing audio behavior. No website script supplies positions or requires
  modification.
- The host embeds a private extension containing only a manifest and collector
  page/script. It extracts exact bytes into a content-addressed directory beneath
  that host's user-data folder, verifies existing files before installation, and
  checks the installed extension's fixed ID. This survives host-executable mirroring
  without loose extension files in the installed mod.
- Each source browser is native-muted before its first navigation and remains
  muted through capture, retries and teardown. Each hidden collector captures only
  its source tab and routes the stream through Web Audio. User mute changes the
  collector graph. Startup, invalid/stale scenes and capture failure remain silent;
  ending capture cannot restore ordinary, non-positional browser output.
- Hidden collectors match their source environment's controller mode. The
  production host uses composition controllers; the original standalone probe
  used ordinary controllers. Mixing those modes caused `ERROR_INVALID_STATE`
  during collector creation in the first game runs. Hidden composition collectors
  have no game presentation or input target.
- The graph downmixes to mono and creates one HRTF panner per supplied TV. It uses
  the main world camera's position and full forward/up basis, the game's master
  volume, and the same maximum of 32 nearest loaded emitters. Linear attenuation is
  full through 1 m and zero at 20 m; normalization limits the combined level from
  TVs mirroring the same page. There is no Wwise room acoustics, occlusion or Doppler,
  and TVs do not gain independent browsing sessions.
- Native scene updates retain their original QPC freshness deadline. The collector
  receives an absolute wall-clock deadline for the remaining lifetime and schedules
  gain to zero on the audio timeline. Updates are coalesced behind one in-flight
  script call, with a two-second unresponsive-collector timeout. An idle landing
  page does not consume the capture-start timeout. Capture waits for successful
  navigation or browser-confirmed direct media; failed capture can recreate its
  collector with at most two retries per navigation. Source and collector
  destruction use weak native callbacks and silence before closing controllers.
- Direct audio/video navigation can commit a playable media document while its
  original HTTP 200 navigation reports `CONNECTION_ABORTED`. That precise case
  requires `DOMContentLoaded` for the same navigation and matching browser-owned
  top-level media metadata before capture may start. The check validates current
  controller, navigation, source, frame/loader identity and URL, rejects error
  pages and stale replies, and expires after two seconds. It does not inject
  script into remote pages or accept arbitrary navigation errors.
- This depends on Chromium's `--allowlisted-extension-id` switch for the one fixed
  extension ID. WebView2 exposes extension installation but does not document this
  capture-permission bypass as a compatibility contract. A runtime update may break
  the route; unsupported API, permission or capture failures leave browser audio
  silent. Developer-mode `browser.getState().audio` exposes route, state, error,
  freshness and logical mute for diagnosis, plus the trusted chrome route and
  camera/emitter snapshots. These describe routing inputs and lifecycle; they are
  not captured-output measurements.

### WebView2 audio evidence and remaining acceptance

The isolated [tab-audio probe](../../../tools/profiling/webview2-tab-audio/README.md)
on WebView2 **155.0.4283.62** establishes capture and spatial-output feasibility:
normal extension invocation is rejected, while the exact-ID allowlist enables tab
capture. Its final controlled-tone run has **49 passing checks**. Captured audio
remains available while direct source output is suppressed; stereo bias,
distance attenuation and restoration are measured at the Windows endpoint.
The public-MP4 run has **47 passing checks and two explicit skips**. That video
plays inside a controlled HTTPS fixture; it does not establish compatibility with
an arbitrary, unmodified remote website. The two skipped level comparisons require
stationary content and are not meaningful for changing video audio.

The subsequent production-route probe compiles the actual host `SpatialAudio.cpp`
and embedded extension assets. Its final run
`build/webview2-tab-audio-probe/runs/20261020-193522-318-allowlist-production/`
passes **99 checks, zero failures or skips**, with compiled source hashes in
`provenance.json`. It verifies native source mute throughout, more than 16 seconds
idle on `about:blank` before capture, HRTF channel direction, linear distance fade,
logical mute, empty/expired scene silence, fresh-scene recovery, navigation while
retaining capture, injected failure silence, teardown and recreation. Failure is
injected through the production route's failure method; this is not a renderer
crash-injection test.

The current game-host run
`build/webview2-spatial-game/runs/20261020-202412-627-webview2-spatial/`
records **211 passing checks, zero failures, zero missing phases and two
conditional geometry skips**. It measures camera-relative panning, master gain,
distance attenuation, mute/range/empty-scene silence, camera independence,
multiple TVs and source continuity through Watch/close. The two skipped checks
compare Watch/close tone amplitude at fixed camera geometry; that geometry was
not stable enough to isolate a presentation effect. Audibility and source
continuity passed separately.

Unmodified public WebM and MP4 URLs also reached a fresh content capture route.
The WebM endpoint windows include Watch/close transitions, so they establish
audio through the observed transitions rather than strictly isolated fullscreen
and world-only intervals. Separate MP4 observations verify logical mute and
recovery. These measurements do not establish gap-free playback, arbitrary-site
or DRM compatibility, or physical Esc/controller behavior. Exact payload/source
hashes and numeric results are in the
[WebView2 validation record](../../../tools/profiling/webview2-game-audio/VALIDATION.md).

The final payload also passed actual collector-crash recovery with both active
MP4 and trusted tone (**1.2705 s**) and with an inactive blank content source
(**0.8806 s** for the trusted tone). Host/browser/source-renderer identities were
preserved. A private save/load produced a new document and two new binding IDs
in **3.839 s**, followed by measured tone output. Cell transfer retired the pinned
TV host by **6.923 s**. These are separate lifecycle observations, not additional
checks in the 211 count; before/after endpoint measurements do not prove a
gap-free crash interval. The first blank-source crash helper false failure and
earlier production failures remain in the validation record.

The game exited normally, the original browser profile was restored with its
file hashes verified, and MO2 returned to Default. The tested host was deployed
to the normal OSF UI mod; its copied hash and the unchanged core match the tested
payload, with zero drift across 42 host-source hashes. The final run's
`deployment.json` supersedes the older `build/webview2-spatial-deployment.json`
for this revision. Endpoint changes, suspend/resume, subjective quality,
latency/audio-video synchronization and broader hardware/input coverage remain
open. Historical CEF acceptance above does not cover the WebView2 output graph.

## Open decisions

The reserved id (`osfui.browser/tv`), local home page and DuckDuckGo default were
confirmed by the user on 2026-10-20.

1. Fullscreen always pauses (implemented). Decide whether to expose that as an option.
2. Plain surfaces can already set `fullscreen: true` (steps 1–3 landed). Decide whether to
   ship that before the browser.
