# OSF UI view toolchain

The fastest authoring loop is a normal npm project. You do not need an OSF UI
source checkout, a hand-written manifest, or a custom Vite configuration.

## Create a view

```bat
npm create osfui@latest my-view
cd my-view
npm run dev
```

The generator asks for a stable mod/view id, Preact or vanilla TypeScript, a
menu or HUD surface, and a Papyrus, native-plugin, settings, or static starting
workflow. The default Preact/Papyrus preset is a useful general starting point.
Generated source uses the production `src/views/<mod>/<view>/` shape.

## Iterate in the browser

`npm run dev` opens the OSF UI harness. Saving HTML, CSS, TypeScript, or TSX
updates the view through Vite HMR. The harness provides the production shared
kit, injects the native bridge before application code, and offers resolution,
visibility, locale, transparency, event injection, and bridge-traffic controls.

Edit `osfui.mock.ts` to provide cached state, localized strings, and
deterministic native/Papyrus responses — request values may also be (async)
functions of the command payload, and named `scenarios` overlay the base
fields (`?scenario=<name>`, or the toolbar's Scenario select). For full
control, export `install(ctx)`: register command handlers ahead of the
scenario engine, push native events, and add your own toolbar controls with
`ctx.registerTools`. The browser reloads when the mock changes. A plain
`osfui.mock.json` fixture keeps working. The mock lives at the project root
so it can never ship with the views.
Use `npm run check` to detect remote URLs and browser transports that the
in-game host does not support.

## Iterate in Starfield

```bat
npm run dev:game -- --deploy "C:\path\to\your\MO2\mod"
```

The first run asks for the root of the mod being authored—the directory
containing `SFSE/`—and remembers it locally. You can also supply `--deploy` as
shown above. The command keeps the browser harness running, rebuilds and syncs
only this project's views after saves, and writes an expiring author-mode
marker beside OSF UI's `config.json`.

Start Starfield while the command is running. Author mode is active without
editing player configuration: F11 reloads the open view and F12 opens WebView2
DevTools. Stopping the command removes the marker; the runtime ignores markers
older than twelve hours after an interrupted session.

To remember the deployment directory, create the ignored local file
`.osfui/local.json`:

```json
{
  "deployRoot": "C:\\path\\to\\your\\MO2\\mod"
}
```

## Build and release

```bat
npm run check
npm run build
npm run package
```

`build` creates `dist/SFSE/Plugins/OSFUI/views/`, generates manifests from
`osfui.config.ts`, and includes the public shared kit. `package` rebuilds and
writes a ready-to-distribute zip under `release/`. `npm run doctor` reports the
active Node version, project root, and discovered views.

