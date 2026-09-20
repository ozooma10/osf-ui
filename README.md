# OSF UI

OSF UI 2.x is the optional WebView add-on for Starfield mods. It owns view
discovery, the JavaScript/native/Papyrus bridge, D3D compositing, focus, and web
input. Settings storage and editing, hotkeys, keybindings, and shared issue reporting
belong to [OSF Settings](https://github.com/ozooma10/osf-settings-slim).

OSF UI stays inert until it acquires ready OSF Settings Slim services at SFSE
`kPostPostLoad` (settings ABI 1.0 and diagnostics ABI 1.0). Its
out-of-process WebView2 helper is created lazily only when a view is demanded.
OSF UI may ship with zero built-in views.

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
- Native view API: [`sdk/OSFUI_Views.h`](sdk/OSFUI_Views.h)
- Papyrus view API: [`data/Scripts/Source/OSFUI_View.psc`](data/Scripts/Source/OSFUI_View.psc)
- Manifest schema: [`docs/schema/manifest.schema.json`](docs/schema/manifest.schema.json)
- Starter: `npm create osfui@latest`

OSF UI never injects Settings data into a page. The owning mod must read OSF
Settings and explicitly publish the minimal values the view needs.

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

The Settings SDK source is pinned by `lib/osf-settings` to Slim commit
`929074399b6f1c584ecefa2d481c9a8c073bd36f`. UI compiles against its public SDK;
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
