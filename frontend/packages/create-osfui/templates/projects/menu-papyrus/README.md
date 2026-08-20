# __OSFUI_PROJECT_NAME__

A small OSF UI menu with a recordless Papyrus backend. The view demonstrates
the normal JavaScript state/event API; its one direct GLOBAL call is the
intentional escape hatch for a script with no quest or alias lifecycle.

## Build

Install the Starfield Creation Kit through Steam (Library > Tools), then run:

```powershell
npm run build
```

The project deliberately keeps the two output trees visible:

- `build-papyrus.ps1` compiles
  `mod/Scripts/Source/__OSFUI_SCRIPT_NAME__.psc` to `mod/Scripts/` using the
  three compiler declarations under `tools/papyrus/`.
- `osfui build` builds the web view to
  `dist/SFSE/Plugins/OSFUI/views/__OSFUI_MOD_ID__/__OSFUI_VIEW_ID__/`.

The current `@osfui/cli` is a view builder; it does not merge those trees or
create an archive. To ship, copy `dist/SFSE` into a staging copy of `mod/`, omit
the `.osfui-build.json` authoring marker, and zip the staging directory so
`Scripts` and `SFSE` are at the archive root.

For a portable or nonstandard game install, run `build-papyrus.ps1` directly
and pass `-StarfieldRoot`, `-PapyrusCompiler`, or `-PapyrusSource`.

## Debug

- Run `npm run dev` for browser hot reload. Edit `osfui.mock.ts` to provide
  test Papyrus data and responses.
- Compile and deploy the backend/settings as a separate MO2 mod:

  ```powershell
  ./build-papyrus.ps1 -Mo2Mods "C:\path\to\MO2\mods"
  ```

- Run `npm run dev:game -- --deploy "C:\path\to\MO2\mods"` to sync the
  generated view into its own authoring mod. Enable both mods in MO2.

`dev:game` mirrors only the view build. It intentionally does not compile,
deploy, or preserve files from the Papyrus backend mod.

The compiled GLOBAL library is discovered on demand when JavaScript calls one
of its functions; there is no plugin to enable and no ESM, startup quest,
alias, or registration to maintain. This is why the starter uses
`osfui.papyrus.call()` for the direct round trip. A quest- or alias-backed
backend should instead register ordinary `OSFUI_View.RegisterSend` or
`RegisterRequest` endpoints after each game load, which JavaScript calls with
`osfui.send()` or `osfui.request()`.

## API references

- `tools/papyrus/OSFUI.psc` — runtime availability and version.
- `tools/papyrus/OSFUI_Settings.psc` — settings and hotkey access.
- `tools/papyrus/OSFUI_View.psc` — send/request endpoints, replies, retained
  state, transient events, and view presentation.
- [Settings authoring guide](https://github.com/ozooma10/osf-ui/blob/main/docs/authoring-settings.md).
