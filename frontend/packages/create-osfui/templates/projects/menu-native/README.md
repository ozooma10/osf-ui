# __OSFUI_PROJECT_NAME__

A runnable OSF UI menu with a native SFSE backend, one example of each bridge
direction, and a small settings schema.

## Build

Add CommonLibSF, then build the backend and view:

```powershell
git submodule add https://github.com/ozooma10/commonlibsf.git native/lib/commonlibsf
npm run build
```

`native/build.mjs` places the DLL under `mod/SFSE/Plugins/`; `osfui build`
places the view under
`dist/SFSE/Plugins/OSFUI/views/__OSFUI_MOD_ID__/__OSFUI_VIEW_ID__/`.

The current `@osfui/cli` does not merge those trees or create an archive. To
ship, copy `dist/SFSE` into a staging copy of `mod/`, omit the
`.osfui-build.json` authoring marker, and zip the staging directory so `SFSE`
is at the archive root.

## Debug

- Run `npm run dev` for browser hot reload. `osfui.mock.ts` mirrors the native
  round trips without loading Starfield.
- Deploy `mod/` as one MO2 mod for the DLL, settings, and localization.
- Run `npm run dev:game -- --deploy "C:\path\to\MO2\mods"` to sync the
  generated view into its own authoring mod. Enable both mods in MO2.

`dev:game` mirrors only the view build; it does not deploy or preserve the
native backend mod.

## Native bridge example

The paired `native/src/main.cpp` and view use the optional `OSFUI_JSON.h`
facade:

- JavaScript `send()` dispatches a typed fire-and-forget `JsonSend`.
- JavaScript `request()` dispatches a `JsonRequest`, and OSF UI owns reply
  correlation and timeout behavior.
- The plugin registers its endpoints, settings/hotkey subscriptions, view, and
  bridge-availability callback.
- The settings schema under `mod/SFSE/Plugins/OSFUI/settings/` is discovered
  automatically; runtime schema registration is deprecated.

The complete native contract is in `native/include/OSFUI_API.h` and
`native/include/OSFUI_JSON.h`. Settings schema details are in the
[settings authoring guide](https://github.com/ozooma10/osf-ui/blob/main/docs/authoring-settings.md).
