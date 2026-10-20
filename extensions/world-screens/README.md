# OSF UI – World Screens

Optional extension for material-backed web displays in Starfield. Development version
0.1.0; not published. Both projects live in this repository.

Install one OSF UI backend edition, then this extension and a content mod that provides
screens. **Use OSF UI from this same source revision**: the previously released 2.1.0
DLL does not export the required `OSFUI_RequestSurfaces` ABI 1.2. The extension checks that
interface at startup and disables itself when it is missing or incompatible.

This extension has one backend-independent DLL, `SFSE/Plugins/OSFUIWorldScreens.dll`.
It uses the browser backend selected by the installed OSF UI edition. It ships its own
`Scripts/OSFUIWorldScreens.pex` and source, with no replacement for `OSFUI.dll`,
`OSFUI.pex`, browser helpers, or the core web kit. Core remains usable without it.

The extension also owns the reserved `osfui.browser/tv` view, its trusted browser
controls and named placeholder texture. Content mods bind to that ID instead of
shipping copies of its pages. Its two-view implementation exists for both editions.
Browsing defaults off: enable **Allow web browsing on World Screens** in
**Mods → OSF UI**, then restart Starfield. Turning it off also takes effect on the next
launch. Websites store cookies and sign-ins in the OSF UI profile folder.
[OSF Browser TV](../browser-tv/README.md) supplies a separate Papyrus-only craftable
wall TV; build it with `pwsh tools/build-browser-tv.ps1 -Deploy`. The new TV and
browser in-game acceptance remain pending. Status:
[Browser surface design](docs/BROWSER_SURFACE.md).

CEF plays browser audio from loaded TVs, using the world camera as the listener.
TVs showing the same page share playback and each contributes a positional emitter.
Sound fades out at 20 m, follows the game's master volume and browser mute, and
stops when the TV unloads. WebView2 currently uses ordinary browser audio.

## Ownership and supported screens

OSF UI owns browser processes, the web/native bridge, shared GPU frame transport, and
exclusive keyboard/controller capture. World Screens owns texture identity hooks,
engine-facing output textures, supported material replacement, reference bindings, and
their game-session lifetime. Content mods own plugins, meshes, generated materials,
web pages, placement, activation rules, and gameplay state.

The current engine hooks are gated to **Starfield 1.16.244.0** and the verified address
and call-site checks in the source. This is not a generic replacement for every vanilla
display. Supported targets are registered ACTI bases using DisplayScreen5, or authored
full-UV screen geometry under `osfui/screens/`. A reference must have a proven private
geometry instance and original material before binding succeeds. Small/medium plugin
form-ID resolution is not implemented; catalogs currently resolve full plugins.

Several references may show the same view. They share one browser and output texture;
different view IDs use separate browsers/profiles and outputs. A view has demand while a
binding exists and the engine has its screen material loaded; bindings in unloaded cells
copy nothing. Browsers start on demand and retire once outstanding GPU reads finish after
the final binding is released, or 10 seconds after the last loaded screen unloads.
The 256 MiB output-texture budget excludes browser/helper memory and shared capture
rings. Live engine resource owners prevent eviction; budget pressure defers new outputs.

## Build and local package

```powershell
xmake build 'OSF UI'
xmake build 'OSF UI - World Screens'
xmake install 'OSF UI - World Screens'
pwsh tools/package-world-screens.ps1
```

Core builds deploy through `XSE_SF_MODS_PATH` into `OSF UI`. Extension builds copy
scripts, docs and view assets; `xmake install 'OSF UI - World Screens'` installs
the extension DLL into its separate mod folder. Unset that variable and `XSE_SF_GAME_PATH` to
build without deployment. The packaging script always disables deployment, creates a
local development archive, verifies its allowlisted payload, and does not publish.
The ordinary core packaging script continues to package core only. Do not swap DLLs
while Starfield is running. Enable the extension in the mod manager only when wanted.

## Author a screen

1. Put a normal OSF UI page in `Data/SFSE/Plugins/OSF/UI/views/<mod>/<view>/`.
2. Generate its named placeholder, materials, and world manifest:

   ```powershell
   node extensions/world-screens/tools/generate-screen.mjs mymod/display ./build/my-screen/Data display-screen5
   ```

   Use `full-uv` for an authored full-UV mesh. The generated manifest defaults to
   1280×720; edit it to the desired resolution, up to 4096 per axis. Copy your page
   into that view folder. The generator refuses to overwrite existing files. View IDs
   are exact, including case; use the same ID in generation, manifests, and binding.
   Keep the generated texture identity: it is derived from the qualified view ID,
   not from its dimensions or pixels. An invalid or mismatched texture identity
   disables only that view; the extension logs the rejected view and keeps valid feeds.
   The `.mat` outputs are MaterialDB JSON authoring
   sources; integrate them through your existing material/asset build workflow.
3. Ship a catalog at `Data/SFSE/Plugins/OSF/WorldScreens/screens/mymod.json`:

   ```json
   {"version":1,"bases":[{"plugin":"MyScreens.esm","formId":"800","profile":"display-screen5"}]}
   ```

   `formId` is hexadecimal local ID without `0x`. A `full-uv` entry also needs
   `"model":"osfui/screens/my-screen.nif"`. The model must actually match the ACTI.
   The extension ships no catalog of vanilla screens and no placed references.
4. Bind from Papyrus after the reference's 3D is ready:

   ```papyrus
   String handle = OSFUIWorldScreens.BindScreen(screenRef, "mymod/display")
   ; Empty means unavailable or unsupported. Rebind after load/3D initialization.
   Bool removed = OSFUIWorldScreens.UnbindScreen(handle)
   ; Views with "fullscreen": true can also open over the game, e.g. from OnActivate:
   Bool queued = OSFUIWorldScreens.OpenFullscreen(handle)
   ```

For native consumers include `sdk/OSFUI_WorldScreens.h` (plus `OSFUI_Surfaces.h` and
`OSFUI.h`) and acquire `OSFUI::API::WorldScreens::Client` after SFSE post-post-load.
`Init()` discovers the service; `IsReady()` checks engine integration. `Bind()` returns
a session-local token. Do not serialize native tokens; save the chosen view ID and
rebind after loading. Papyrus handles additionally carry a process UUID to reject old saves.

Native `BeginInteraction(binding, callback, context)` requests exclusive input. Admission
waits for the selected material, a completed frame, neutral keys/buttons, and available
core input integration. The callback reports `Started` then `Ended`, or `Rejected`.
Keep its context alive until the terminal callback; callbacks run on the core runtime tick.
The page receives `ui.interaction` with `active`, and the ordinary `ui.gamepad` events.
Use `osfui.gamepadMode` for raw controller input. Escape, controller Back, menu opening,
focus loss, invalidation, and browser failure end capture. In-world interaction does not
pause gameplay or provide mouse pointing. Content owns proximity/combat/activation policy.

ABI 1.1 adds `BeginInteractionWith(binding, options, callback, context)` (the `Client`
overload taking `InteractionOptions`). `Presentation::Fullscreen` behaves like a pausing
menu: the cursor is shown, the screen's page is drawn letterboxed over the game at its
manifest resolution, and the mouse is routed to it. The page keeps running on the world
screen when the interaction ends. Only views whose manifest sets `"fullscreen": true` are
admitted; others end `Rejected` with reason `fullscreen-unsupported`. Papyrus uses
`OSFUIWorldScreens.OpenFullscreen(handle)`. Choose a manifest resolution that stays legible
fullscreen; it is never resized.

## Migrate prototype consumers

- Replace `OSFUI_Screens.h` and its prototype namespace with `OSFUI_WorldScreens.h`
  and `OSFUI::API::WorldScreens`. Rebuild native consumers; the old export is not aliased.
- Replace `OSFUI.BindScreen` / `OSFUI.UnbindScreen` with `OSFUIWorldScreens.*` and
  recompile Papyrus. Keep ordinary OSF UI message/state calls in `OSFUI`.
- Move catalogs from `OSF/UI/screens` to `OSF/WorldScreens/screens`. Keep view folders
  in `OSF/UI/views`, and keep existing generated texture/material paths.
- Replace the monolithic prototype OSFUI DLL with the matching current core plus this
  extension. Do not install two core editions or copy the old core over the new one.

`tools/world_content.py` provides the shared BA2, NIF and plugin authoring helpers
used by the migrated Starcade and SSSE builders. It preserves placement hierarchy
and collision blocks while authoring the selected display quad. Decorative screen
overlays are detached from their parent nodes; their hidden flag alone is insufficient
to prevent them from covering the web image in Starfield.
The helper escapes dots as `_2e` and underscores as `_5f` in external `.mesh`
directory names: `x2357.ssse` becomes `geometries/osfui/screens/x2357_2essse/`.
This keeps distinct owner IDs such as `example.mod` and `example_mod` from
overwriting each other's meshes. The original dotted mesh path failed to draw
in the verified game run. Qualified view IDs, texture identities and NIF/catalog
paths keep their existing names. Regenerate and repackage content built with
the earlier dot-to-underscore mapping: both its generated NIFs and external
meshes must use the new paths together.

## Validation scope

Native lifecycle tests cover optional startup, shared demand, API version rejection,
input admission/cancellation, fullscreen admission, pause, letterboxed pointer mapping,
and GPU-busy retirement using engine/GPU doubles. The fullscreen draw itself is GPU work
and is only verified in game.
Binding tests cover private geometry, replacement, invalidation, and save/load tokens.
Node tests cover generated texture identities and material profiles. Build and package
checks are separate from actual GPU, input, and save/load acceptance in Starfield.
The old monolithic prototype's runtime results do not validate these newly split DLLs.

The migrated addon runner is documented in [tests/game](tests/game/README.md).
The [2026-10-19 addon validation](ADDON_VALIDATION.md) records fresh Starcade and
SSSE WebView2 and CEF game results, keyboard/focus checks, ship workshop placement,
deployment hashes and the remaining acceptance gaps.

Before release, exercise core alone and core plus extension with each browser edition:
simultaneous/mirrored feeds, overlay switching, controller/keyboard exit, focus loss,
helper failure, cell/3D unload/reload, save/load, and removal of optional content.
