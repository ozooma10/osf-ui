# Project templates

Each child directory is the project copied for one supported
`surface`/`integration` preset. The scaffolder only replaces explicit
`__OSFUI_*__` tokens in file contents and paths.

The supported presets are `menu-papyrus`, `menu-native`, and
`settings-papyrus`. Do not expose a new surface/integration choice until its
authored directory and end-to-end scaffold test both exist.

Menu views are deliberately static and directly deployable. Each one lives at
`mod/SFSE/Plugins/OSFUI/views/<mod>/<view>/` and contains only `index.html`,
`main.js`, `style.css`, and `manifest.json`. They use classic script and
stylesheet tags: no TypeScript, package manifest, dependency install, config
module, mock runtime, bundler, or other frontend build step belongs in these
baseline starters. Tool-assisted projects can be added later as separate,
advanced presets.

Every browser starter uses the current public bridge vocabulary:
`osfui.send()` and `osfui.request()` for browser-to-host endpoints,
`osfui.on()` for transient events, and `osfui.state.on()` for retained state.
An owning view uses local endpoint, event, and state names; qualification is
reserved for cross-mod or platform addresses. Registered Papyrus endpoints can
be called with variadic arguments, such as `osfui.request("sum", 2, 3)`, which
the helper sends as `{ args: [2, 3] }`. `OSFUI_View.Reply` becomes the raw value
resolved by `request()`.

The recordless starter calls the platform's `papyrus.call` endpoint through
`osfui.send()` because it has no load lifecycle in which to register an
`OSFUI_View` endpoint. It does not add a second Papyrus-specific JavaScript API.

The three Papyrus compiler APIs (`OSFUI.psc`, `OSFUI_Settings.psc`, and
`OSFUI_View.psc`) are copied from `data/Scripts/Source/`; the native API headers
are copied from `sdk/`. They are canonical repository sources, not project
templates. Package publishing stages them as npm payload files so the installed
scaffolder has the same inputs without keeping synchronized source mirrors.
