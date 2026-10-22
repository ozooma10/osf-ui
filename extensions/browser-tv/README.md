# OSF Browser TV

A craftable wall TV for World Screens. Activate **Watch** to open its browser
fullscreen, choose a website, then close it to keep playing on the TV.

Development content mod: Papyrus and records only, with no DLL. Requires the
matching unreleased **OSF UI** core and **OSF UI - World Screens** extension, plus
their SFSE, Address Library and OSF Settings dependencies. Released OSF UI 2.1.0
does not include the required browser surface APIs.

## Install and use

1. Install this mod alongside the matching core and World Screens. Enable
   `OSFBrowserTV.esm` in your mod manager.
2. In **Mods → OSF UI - World Screens → Browsing**, enable **Allow web browsing on
   World Screens**, then restart Starfield. With consent off, the TV shows the local
   disabled page and stays offline.
3. In a ship's Decorate menu or an outpost's build menu, choose **Decorations →
   Browser TV**. The recipe uses 4 Aluminum, 2 Zero Wire and 2 Beryllium, matching
   the existing craftable Starcade TV.
4. Place it on a wall, leave build mode, and activate **Watch**. Use the address
   bar or the channel guide for a site or a search. **Return to world** or Esc goes
   back to the game; Esc first leaves a site's own fullscreen mode or the guide.

All Browser TVs mirror the same browser and share its pinned channels, recent
pages, search engine, last address, cookies and mute state. Closing fullscreen keeps playback running.
Leaving the area or removing the last loaded TV retires the browser; the next
loaded TV restores the last address, not the playback position. Both browser
editions support spatial audio from nearby loaded TVs, with camera-relative
positioning and distance attenuation. DRM services are unsupported, and codec
support varies by browser edition.
**Channel settings** in the guide can turn the channel off or clear cookies and sign-ins.

The TV uses the vanilla DisplayScreen5 model and the existing wall placement
data. It adds no placed objects or edits to vanilla records. Watch activation,
public video playback and continuing after Esc passed a basic WebView2 in-game
check. Workshop crafting, placement/movement/removal, save/load and CEF still need
in-game acceptance for this new mod.

## Build

From the OSF UI repository, with Starfield closed:

```powershell
pwsh tools/build-browser-tv.ps1
# Also copy only this mod's payload files to XSE_SF_MODS_PATH/OSF Browser TV:
pwsh tools/build-browser-tv.ps1 -Deploy
```

The builder uses the installed `Starfield.esm` as a read-only source and the CK
Papyrus compiler. Override `-Game`, `-Compiler`, `-Imports` or `-ModsPath` as needed.
`PAPYRUS_COMPILER` and `PAPYRUS_IMPORTS` are also supported. Every build uses a fresh
directory under `build/browser-tv`; `latest-build.json` records its path and file
hashes. It validates the generated records, script attachment, catalog and workshop
data before deployment. It does not enable the mod, change a profile or launch the game.

The payload contains `OSFBrowserTV.esm`, `OSFBrowserTV.pex`, its `.psc` source, the
screen catalog and notices. World Screens supplies the browser view, materials,
textures and API script; this mod does not bundle framework files or vanilla assets.

| Record | Local FormID | Purpose |
| --- | --- | --- |
| `OSFBrowserTV` | `800` | Craftable DisplayScreen5 ACTI; `OSFBrowserTV` script |
| `co_Outpost_Decor_OSFBrowserTV` | `801` | Decorations recipe |
| `OSFBrowserTVWatch` | `802` | “Watch” activation text |

The plugin is a full ESM with only `Starfield.esm` as its master. The catalog
registers local `800` with profile `display-screen5`. Papyrus binds the exact
reserved view `osfui.browser/tv` on load/cell attach, after save load and workshop
placement/movement. Initialization retries for up to ten seconds. Activation
retries binding and requests fullscreen; it never queues a delayed automatic open.
Unload/detach/removal cancels retries and releases the handle. Repeated binding
does not retire the shared browser.

See the [browser design and verification status](../world-screens/docs/BROWSER_SURFACE.md)
in the source repository for the remaining browser-runtime acceptance and limitations.
