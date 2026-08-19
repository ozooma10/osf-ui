# __OSFUI_PROJECT_NAME__

A runnable OSF UI menu starter with one small JavaScript-to-Papyrus round trip.
It is deliberately not a catalogue — the documentation linked below covers the
rest.

The view uses only `index.html` and plain, browser-ready `main.js`. Its CSS is
inline, and there is no framework, TypeScript, or transpilation step while you
edit or preview it. The one import in `main.js` is OSF UI's bridge API.

## Build

1. Run `npm run doctor` and install any missing Creation Kit prerequisites.
2. Run `npm run build` to compile the loose PEX and assemble the view into the
   installable mod layout.
3. Run `npm run package` to create the plugin-free installable zip in
   `release/`.

## Debug

- Run `npm run dev` to test the view in a browser with hot reload. Edit
  `osfui.mock.ts` to provide test Papyrus data and responses.
- Run `npm run dev:game -- --deploy "path-to-MO2-mods"` to test in Starfield.
  Instantiated views reload automatically; press F12 to open DevTools.

The Papyrus library is
`mod/Scripts/Source/__OSFUI_SCRIPT_NAME__.psc`. Its compiled PEX is discovered
on demand when JavaScript calls one of its GLOBAL functions; there is no plugin
to enable and no ESM, startup quest, alias, or registration to maintain.

## Where to read more

- [authoring-views.md](https://github.com/ozooma10/osf-ui/blob/main/docs/authoring-views.md) — the full bridge protocol:
  every platform endpoint, event, and lifecycle rule.
- [authoring-settings.md](https://github.com/ozooma10/osf-ui/blob/main/docs/authoring-settings.md) — every settings
  control, widget, predicate, preset, and localization address.
- [authoring-dynamic-data.md](https://github.com/ozooma10/osf-ui/blob/main/docs/authoring-dynamic-data.md) — a worked
  state-and-event example between a mod backend and a view.
- The copied `tools/papyrus/OSFUI.psc` — every OSF UI Papyrus function
  with its contract in the comments.
- [view-toolchain.md](https://github.com/ozooma10/osf-ui/blob/main/docs/view-toolchain.md) — the CLI, the browser
  harness, deployment, and packaging.
