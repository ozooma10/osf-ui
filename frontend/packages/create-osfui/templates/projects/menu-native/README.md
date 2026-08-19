# __OSFUI_PROJECT_NAME__

A runnable OSF UI menu starter: one worked example of each way a
view and its mod backend talk to each other, plus a small settings schema. It is
deliberately not a catalogue — the documentation linked below covers the rest.

Run `npm run dev` for instant browser HMR. Run `npm run dev:game -- --deploy "path-to-MO2-mods"`
to create this mod's folder under MO2 and sync into Starfield with temporary
developer mode via an author-mode marker, automatic view reload, and F12 DevTools.

Use `npm run package` to create a release-ready zip. Files under `mod/`
are copied into the mod archive beside the generated view.

## Native SFSE mod backend

The paired `native/src/main.cpp` and view source are an end-to-end bridge
example built on the optional `OSFUI_JSON.h` facade:

- **Send message to C++** sends a typed fire-and-forget `JsonSend`; C++
  changes its state and pushes the serialized struct back to JavaScript.
- **Call C++ and await reply** sends a `JsonRequest`; C++ validates the
  required `name`, replies with JSON, and lets OSF UI own correlation.
- The plugin also registers this view, settings and bridge-availability
  callbacks, and an **F9** open-view hotkey. The bundled
  `mod/SFSE/Plugins/OSFUI/settings/__OSFUI_MOD_ID__.json` schema is discovered
  automatically; runtime schema registration is deprecated. `osfui.mock.ts`
  mirrors the round trips in the browser harness.

1. Install xmake and Visual Studio's C++ workload.
2. Add CommonLibSF: `git submodule add https://github.com/ozooma10/commonlibsf.git native/lib/commonlibsf`.
3. Run `npm run build:native`. xmake fetches nlohmann/json and puts the DLL in `mod/SFSE/Plugins/`.
4. Run `npm run package` to build the DLL and view into one mod archive.

## Where to read more

- [authoring-views.md](https://github.com/ozooma10/osf-ui/blob/main/docs/authoring-views.md) — the full bridge protocol:
  every platform endpoint, event, and lifecycle rule.
- [authoring-settings.md](https://github.com/ozooma10/osf-ui/blob/main/docs/authoring-settings.md) — every settings
  control, widget, predicate, preset, and localization address.
- [authoring-dynamic-data.md](https://github.com/ozooma10/osf-ui/blob/main/docs/authoring-dynamic-data.md) — a worked
  state-and-event example between a mod backend and a view.
- [native-plugin-api.md](https://github.com/ozooma10/osf-ui/blob/main/docs/native-plugin-api.md) and the copied
  `native/include/OSFUI_API.h` — the complete C ABI.
- [view-toolchain.md](https://github.com/ozooma10/osf-ui/blob/main/docs/view-toolchain.md) — the CLI, the browser
  harness, deployment, and packaging.
