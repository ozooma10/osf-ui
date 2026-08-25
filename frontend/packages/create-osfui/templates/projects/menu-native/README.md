# __OSFUI_PROJECT_NAME__

A directly deployable OSF UI menu with a native SFSE backend. The view is plain
`index.html`, `main.js`, and `style.css`; it has no TypeScript, npm dependencies,
config module, or frontend build step.

The browser files and `manifest.json` already live in their final location:

`mod/SFSE/Plugins/OSFUI/views/__OSFUI_MOD_ID__/__OSFUI_VIEW_ID__/`

## Build the native plugin

Install [xmake](https://xmake.io/), add CommonLibSF, then build and install the
DLL into `mod/`:

```powershell
git submodule add https://github.com/ozooma10/commonlibsf.git native/lib/commonlibsf
xmake f -P . -m releasedbg
xmake build -P .
xmake install -P .
```

There is nothing to build for the web view. Deploy or archive `mod/` with
`SFSE` at the mod root.

## Debug

Deploy `mod/`, open the generated view in Starfield, and use F12 for Chromium
DevTools. Edit the HTML, CSS, or JavaScript in the deployed view and reload the
page to iterate. The runtime supplies `/shared/osfui.js`, `/shared/osfui.css`,
and the optional `/shared/gamepadnav.js` from the common
`https://osfui.example` origin; do not copy or bundle them into the view.

## Native bridge example

The paired `native/src/main.cpp` and view use the optional `OSFUI_JSON.h`
facade:

- The owning view calls local endpoints such as `increment`, `getState`, and
  `greet`; the plugin keeps the native ABI's qualified registrations.
- `osfui.send()` is one-way. `osfui.request()` resolves the raw reply payload
  or rejects with a stable error code.
- `osfui.on("notice", ...)` receives transient events, while
  `osfui.state.on("state", ...)` receives retained state and its immediate
  replay after a document reload.
- The plugin registers its endpoints, settings/hotkey subscriptions, view, and
  bridge-availability callback.
- The settings schema under `mod/SFSE/Plugins/OSFUI/settings/` is discovered
  automatically.

Each native service header is standalone; include Views, Settings, or
Diagnostics as needed. `OSFUI_API.h` is legacy ABI 1 compatibility.
Settings schema details are in the
[settings authoring guide](https://github.com/ozooma10/osf-ui/blob/main/docs/authoring-settings.md).
