# OSF UI

OSF UI 2.x is the optional WebView add-on for Starfield mods. It owns view
discovery, the JavaScript/native/Papyrus bridge, D3D compositing, focus, and web
input. Settings storage and editing, hotkeys, keybindings, and shared issue reporting
belong to [OSF Settings](https://github.com/ozooma10/osf-settings-slim).

OSF UI stays inert until it acquires ready OSF Settings Slim services at SFSE
`kPostPostLoad` (settings ABI 1.0 and diagnostics ABI 1.0). Its
out-of-process WebView2 helper is created lazily only when a view is demanded.
OSF UI may ship with zero built-in views.
For an in-game test panel, install the separate [settings example](examples/settings-view/README.md)
and press F6. Enabling OSF UI alone does not add a menu or hotkey.

The current [forwarded-input prototype](docs/cdp-input-prototype.md) keeps native
focus in Starfield and sends physical keyboard/text input through Chromium's
DevTools Protocol. The helper process and shared-texture rendering remain in use.

## Runtime layout

```text
Data/SFSE/Plugins/OSFUI.dll
Data/SFSE/Plugins/OSF/UI/bin/osfui_webview2_host.exe
Data/SFSE/Plugins/OSF/UI/views/<mod-id>/<view-name>/
Data/SFSE/Plugins/OSF/Settings/schemas/osfui.json
```

The archive owns `OSF/UI` and exactly the `osfui.json` schema. It never cleans
the shared `OSF` parent or the OSF Settings sibling subtree.

## Authoring

- Web bridge types: [`sdk/osfui.d.ts`](sdk/osfui.d.ts)
- Native API: [`sdk/OSFUI.h`](sdk/OSFUI.h), `OSFUI::API::Client` via `OSFUI_RequestAPI`
- Papyrus API: [`data/Scripts/Source/OSFUI.psc`](data/Scripts/Source/OSFUI.psc)
- Manifest schema: [`docs/schema/manifest.schema.json`](docs/schema/manifest.schema.json)
- Material-backed world views: [`docs/world-surfaces.md`](docs/world-surfaces.md)
- Starter: `npm create osfui@latest`

OSF UI never injects Settings data into a page. The owning mod must read OSF
Settings and explicitly publish the minimal values the view needs.

Menu views can opt into the OSF Settings mod launcher with
`"launcher": { "modId": "mymod", "modTitle": "My Mod" }` in their manifest.
All opted-in views appear together in the top-level Launcher tab. Use your lowercase OSF mod ID as the owner.
The view's title and description label the destination; its qualified view ID
remains unchanged. Omit `launcher` for private views. HUD/world views cannot opt
in, and debug-only entries appear only with developer mode enabled. This uses
the optional launcher ABI 2.0; direct view APIs still work with older Settings.
Discovery registers metadata without starting WebView2. Opening uses the normal
RequestMenu preflight/load/input pipeline. The view owns closing; players reopen
Settings through its normal hotkey or menu entry.

## Build and package

```powershell
git submodule update --init --recursive
pwsh tools/setup.ps1
xmake f -P . -m releasedbg
xmake build -P . -y "OSF UI"
npm run verify
pwsh tools/package.ps1
```

The runtime targets Starfield 1.16.244 and requires SFSE, Address Library, OSF
Settings Slim with the SDK interfaces above, and the Edge WebView2 Evergreen Runtime.

Deploying or packaging compiles the single `OSFUI.psc` into `build/papyrus/OSFUI.pex`.
Install the Creation Kit Papyrus compiler and vanilla imports; set `PAPYRUS_COMPILER`
and `PAPYRUS_IMPORTS` if they are outside the defaults in `tools/build-papyrus.ps1`.
Native builds without a deployment target do not require the Papyrus toolchain.

The Settings SDK source is pinned by `lib/osf-settings` to Slim commit
`2330e02e9922da688e5c85d3a1a8b981bfa8a6d6`. UI compiles against its public SDK;
it does not build or distribute OSF Settings. The Slim repository currently
requires authenticated access. CI accepts an `OSF_DEPENDENCIES_TOKEN` secret
with read access to the dependency repository; ordinary development checkouts
need equivalent Git credentials. The older extracted Settings
project uses different exports and cannot satisfy this dependency.

OSF UI reads `developerMode` and `highRefreshCapture` once at startup. Both
require a game restart; failed reads use `false` and report a Mod Issue.
WebView failure detection and recovery remain in UI. Settings receives current,
readable issue reports; technical details stay in the native log. Input capture
holds a Slim hotkey block, and closes if that block cannot be acquired.

The [development example](examples/settings-view/README.md) demonstrates a
Slim hotkey opening a view and a setting explicitly forwarded by its native owner.
It is excluded from the production package. Settings Papyrus APIs, actions, and
localization services are not part of this first migration.

Host checks (Bash 4+ and a C++23 compiler):

```sh
bash tests/native/run.sh
bash tests/schema/run.sh
npm run verify
```

The schema check compiles Slim's actual parser against the runtime and example boolean
schemas. Its portable stubs abort if string conversion or native key lookup is
used; Windows CI is configured to validate the same schemas with the Windows SDK. On Node 26,
use `NODE_OPTIONS=--no-experimental-webstorage npm run verify` for jsdom tests;
CI uses Node 22.

See the [verification record](docs/SETTINGS-INTEGRATION-VERIFICATION.md) for completed
checks and remaining Windows/package prerequisites. See [`MIGRATION.md`](MIGRATION.md) for the intentional 1.x compatibility break.

## License

OSF UI is licensed under [GPL-3.0](LICENSE), with the additional permissions in
[EXCEPTIONS](EXCEPTIONS). See [CREDITS.md](CREDITS.md).
