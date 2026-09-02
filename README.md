# OSF UI

OSF UI 2.x is the optional WebView add-on for Starfield mods. It owns view
discovery, the JavaScript/native/Papyrus bridge, D3D compositing, focus, and web
input. Settings, hotkeys, keybindings, localization, actions, and diagnostics
belong to [OSF Settings](https://github.com/ozooma10/osf-settings).

OSF UI stays inert until it acquires a compatible OSF Settings 1.x service. Its
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
Settings `>=1.0.0 <2.0.0`, and the Edge WebView2 Evergreen Runtime.

See [`MIGRATION.md`](MIGRATION.md) for the intentional 1.x compatibility break.

## License

OSF UI is licensed under [GPL-3.0](LICENSE), with the additional permissions in
[EXCEPTIONS](EXCEPTIONS). See [CREDITS.md](CREDITS.md).
