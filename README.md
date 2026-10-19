# OSF UI

A web view framework for Starfield.

OSF UI **2.1.0** is prepared as separate WebView2 and CEF editions from one codebase,
with edition-specific DLLs and a shared API. See the [2.1 release notes](docs/release-2.1.md) for changes and upgrade requirements.

**Making a mod with OSF UI? Start with the [authoring guide](docs/authoring.md).**

| Reference | Use it to |
| --- | --- |
| [osfui.d.ts](sdk/osfui.d.ts) | Call `send`, `request`, `on`, and `state` from a page |
| [OSFUI.h](sdk/OSFUI.h) | Open views, handle messages, and publish state from C++ (`OSFUI::API::Client` via `OSFUI_RequestAPI`) |
| [OSFUI.psc](data/Scripts/Source/OSFUI.psc) | The same from Papyrus |
| [manifest.schema.json](docs/schema/manifest.schema.json) | Describe a view |


## Runtime

There are 2 different backend editions for OSFUI, recommended Webview2, and CEF which bundles the runtime.

Both editions require SFSE, Address Library, and OSF Settings

| Edition | Recommendation | Browser requirement |
| --- | --- | --- |
| **WebView2 - recommended default** | Choose this for most installations: smaller download and lower measured browser CPU and memory overhead. | Microsoft WebView2 Evergreen Runtime |
| **CEF - bundled-runtime alternative** | Choose this when you want the browser runtime included with OSF UI. It has a larger download and higher measured CPU and memory overhead. | Bundled Chromium |

## Build and test

Use XMake, an MSVC compiler with C++23 support. 

```powershell
git submodule update --init --recursive
pwsh tools/setup.ps1
xmake f -P . -m releasedbg --browser_backend=webview2
xmake build
```

Deploy and package builds also compile `OSFUI.psc`; set `PAPYRUS_COMPILER` and `PAPYRUS_IMPORTS` if the Creation Kit compiler is outside the defaults in `tools/build-papyrus.ps1`.

## License

[GPL-3.0](LICENSE) with the additional permissions in [EXCEPTIONS](EXCEPTIONS).
