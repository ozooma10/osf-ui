# OSF UI

OSF UI is a Mod Settings and Web interface framework for Starfield. It lets mods present html views in-game and communicate to and from Papyrus or native SFSE

The runtime provides shared settings and keybinds, localization, and player-facing health diagnostics. 

An isolated WebView2 helper renders pages out of process and shares their frames with Starfield.

Mod authors can start with the concise [settings authoring guide](docs/authoring-settings.md).

## Build

```powershell
git submodule update --init --recursive
pwsh tools/setup.ps1
xmake f -m releasedbg
xmake build
```

Run `npm run verify` for the frontend checks. To create a mod-manager-ready archive under `dist/`, run:

```powershell
pwsh tools/package.ps1
```

The packaged mod requires [SFSE](https://sfse.silverlock.org/), the Starfield Address Library, and the Edge WebView2 Evergreen Runtime.

## License

OSF UI is licensed under [GPL-3.0](LICENSE), with the additional permissions in [EXCEPTIONS](EXCEPTIONS). 

See [CREDITS.md](CREDITS.md) for third-party acknowledgments.
