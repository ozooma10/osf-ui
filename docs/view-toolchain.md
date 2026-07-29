# OSF UI view toolchain

The fastest authoring loop is a normal npm project. You do not need an OSF UI
source checkout, a hand-written manifest, or a custom Vite configuration.

## Create a view

```bat
npm create osfui@latest my-view
cd my-view
npm run dev
```

The generator asks for a stable mod/view id, a menu or HUD surface, and a
Papyrus or native-plugin starting workflow. Projects are TypeScript with
strict checking and no UI framework dependency; hand-written plain `.js`
modules also build (`allowJs`). Generated source uses the production `src/views/<mod>/<view>/` shape and the
selected workflow adds its matching backend starter: Papyrus source or a native
SFSE/CommonLibSF plugin project.

## Iterate in the browser

`npm run dev` opens the OSF UI harness. Saving HTML, CSS, TypeScript, or JavaScript
updates the view through Vite HMR. The harness provides the production shared
kit, injects the native bridge before application code, and offers resolution,
visibility, locale, transparency, event injection, and bridge-traffic controls.

Edit `osfui.mock.ts` or `osfui.mock.js` to provide cached state, localized strings, and
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
npm run dev:game -- --deploy "C:\path\to\MO2\mods"
```

The first run asks for MO2's `mods` directory and remembers it locally. It
creates a child mod folder matching the project directory name, then places the
generated `SFSE/` tree inside it. You can also supply `--deploy` as shown above.
The command keeps the browser harness running, rebuilds and syncs only this
project's views after saves, and writes an expiring author-mode marker beside
OSF UI's `config.json`.

Start Starfield while the command is running. Author mode is active without
editing player configuration: F11 reloads the open view and F12 opens WebView2
DevTools. Stopping the command removes the marker; the runtime ignores markers
older than twelve hours after an interrupted session.

To remember the deployment directory, create the ignored local file
`.osfui/local.json`:

```json
{
  "modsRoot": "C:\\path\\to\\MO2\\mods"
}
```

## Build and release

```bat
npm run check
npm run build
npm run package
```

`build` starts by copying the project's `mod/` Data-root tree into `dist/`,
then creates `dist/SFSE/Plugins/OSFUI/views/`, generates manifests from
`osfui.config.ts` or `osfui.config.js`, and includes the public shared kit. Put compiled Papyrus
scripts, native DLLs, settings schemas, and other normal mod files under
`mod/`; they are included by `build`, `package`, and the initial `dev:game`
sync. `package` rebuilds and writes a ready-to-distribute zip under `release/`.
`npm run doctor` reports the active Node version, project root, and discovered
views. Set `modRoot` in your `osfui.config.*` only when your Data-root source tree
uses a different directory name.

`check`, `doctor`, `build`, `package`, and every `dev:game` sync also compare
each `mod/Scripts/Source/User/**/*.psc` against its compiled
`mod/Scripts/**/*.pex` and warn when the `.pex` is missing or older than its
source — the game only loads compiled scripts, so a forgotten compile would
otherwise fail silently in game. The warnings are advisory (the Papyrus
compiler ships with the Creation Kit, so the CLI cannot run it for you) and
never fail a build.
