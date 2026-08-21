# __OSFUI_PROJECT_NAME__

A directly deployable OSF UI menu with a recordless Papyrus backend. The view
is plain `index.html`, `main.js`, and `style.css`; it has no TypeScript, npm
dependencies, config module, or frontend build step.

The browser files and `manifest.json` already live in their final location:

`mod/SFSE/Plugins/OSFUI/views/__OSFUI_MOD_ID__/__OSFUI_VIEW_ID__/`

## Build the Papyrus script

Install the Starfield Creation Kit through Steam (Library > Tools), then run:

```powershell
./build-papyrus.ps1
```

The script compiles `mod/Scripts/Source/__OSFUI_SCRIPT_NAME__.psc` to
`mod/Scripts/` using the three compiler declarations under `tools/papyrus/`.
There is nothing to build for the web view. Deploy or archive `mod/` with
`Scripts` and `SFSE` at the mod root.

For a portable or nonstandard game install, pass `-StarfieldRoot`,
`-PapyrusCompiler`, or `-PapyrusSource`. To compile and copy the complete mod
into MO2 in one command, run:

```powershell
./build-papyrus.ps1 -Mo2Mods "C:\path\to\MO2\mods"
```

## Debug

Open the generated view in Starfield and use F12 for Chromium DevTools. Edit
the HTML, CSS, or JavaScript in the deployed view and reload the page to
iterate. The runtime supplies `https://osfui-assets.example/osfui.js` (plus
`osfui.css` and optional `gamepadnav.js` at the same origin); do not copy or
bundle those files into the view.

The compiled GLOBAL library is discovered on demand when JavaScript calls one
of its functions; there is no plugin to enable and no ESM, startup quest,
alias, or registration to maintain. This is why the starter uses
the platform's `papyrus.call` endpoint through `osfui.send()` for the direct
round trip. A quest- or alias-backed backend should instead register ordinary
`OSFUI_View.RegisterSend` or
`RegisterRequest` endpoints after each game load. Its owning JavaScript calls
them with `osfui.send("localName", ...args)` or
`osfui.request("localName", ...args)`. `OSFUI_View.Reply` becomes the raw value
resolved by the JavaScript promise. Use a qualified endpoint only when
intentionally addressing another mod.

The starter's retained `clicks` value and transient `notice` event are consumed
through their local owner names: `osfui.state.on("clicks", ...)` and
`osfui.on("notice", ...)`.

## API references

- `tools/papyrus/OSFUI.psc` — runtime availability and version.
- `tools/papyrus/OSFUI_Settings.psc` — settings and hotkey access.
- `tools/papyrus/OSFUI_View.psc` — send/request endpoints, replies, retained
  state, transient events, and view presentation.
- [Settings authoring guide](https://github.com/ozooma10/osf-ui/blob/main/docs/authoring-settings.md).
