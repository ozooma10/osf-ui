# OSF UI

A web view framework for Starfield.

OSF UI **2.1.0** supports 2 different backends, WebView2 and CEF. See [Runtime](#runtime) for edition requirements.

**Making a mod with OSF UI? Start with the [authoring guide](docs/authoring.md).**

| Reference | Use it to |
| --- | --- |
| [osfui.d.ts](sdk/osfui.d.ts) | Call `send`, `request`, `on`, and `state` from a page |
| [OSFUI.h](sdk/OSFUI.h) | Open views, handle messages, and publish state from C++ (`OSFUI::API::Client` via `OSFUI_RequestAPI`) |
| [OSFUI.psc](data/Scripts/Source/OSFUI.psc) | The same from Papyrus |
| [manifest.schema.json](docs/schema/manifest.schema.json) | Describe a view |


## Runtime

There are 2 different backend editions for OSFUI, recommended Webview2, and CEF which bundles the runtime.

Both editions require SFSE, Address Library, and OSF Settings 1.2.0 or a compatible newer release.

| Edition | Recommendation | Browser requirement |
| --- | --- | --- |
| **WebView2 - recommended default** | Choose this for most installations: smaller download and lower measured browser CPU and memory overhead. | Microsoft WebView2 Evergreen Runtime |
| **CEF - bundled-runtime alternative** | Choose this when you want the browser runtime included with OSF UI. It has a larger download and higher measured CPU and memory overhead. | Bundled Chromium |

## Optional World Screens extension

[OSF UI - World Screens](extensions/world-screens/README.md) adds material-backed
in-world displays in a separate DLL and package. It shares this repository and uses
core's browser and input services. The extension is an unpublished development build;
it requires the matching core source revision. Ordinary core installs do not include it.

[OSF Browser TV](extensions/browser-tv/README.md) is its Papyrus-only craftable
browser TV consumer. It builds separately with `pwsh tools/build-browser-tv.ps1`;
web browsing requires the default-off consent setting and a game restart.

## Build and test

Use XMake, an MSVC compiler with C++23 support. 

```powershell
git submodule update --init --recursive
pwsh tools/setup.ps1
xmake f -P . -m releasedbg --browser_backend=webview2
xmake build
```

Deploy and package builds also compile `OSFUI.psc`; set `PAPYRUS_COMPILER` and `PAPYRUS_IMPORTS` if the Creation Kit compiler is outside the defaults in `tools/build-papyrus.ps1`.

Run the desktop native tests with `bash tests/native/run.sh` (Git Bash on Windows;
Python 3.9+ and a C++23 compiler are required). The runner uses Clang/GCC when available,
or MSVC through Visual Studio. It caches objects and executables in `tests/native/.build`,
tracks source and included-header contents plus compiler settings, and rebuilds only
affected objects and suites. Every invocation still runs all tests. Use
`bash tests/native/run.sh --clean` to force a full rebuild. The small runner regression
check is `python tests/native/test_build_runner.py`.

## License

[GPL-3.0](LICENSE) with the additional permissions in [EXCEPTIONS](EXCEPTIONS).
