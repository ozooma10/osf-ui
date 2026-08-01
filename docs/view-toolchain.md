# OSF UI view toolchain

Authoring a view is a normal npm project. No OSF UI source checkout, hand-written manifest, or custom Vite config needed.

## Create a view

```bat
npm create osfui@latest my-view
cd my-view
npm run doctor
npm run dev
```

The generator asks for a mod/view id, a menu or HUD surface, and a Papyrus or native-plugin starter. Projects are TypeScript (strict, no UI framework); plain `.js` modules build too (`allowJs`). Source uses the production `src/views/<mod>/<view>/` shape, and the chosen workflow adds its backend starter — Papyrus source, or a native SFSE/CommonLibSF plugin project. The Papyrus preset also ships Spriggit text source for a Start Game Enabled quest and player alias, so it builds a real ESM instead of leaving record setup as a Creation Kit chore.

## Iterate in the browser

`npm run dev` opens the OSF UI harness: production shared kit, native bridge injected before your code, Vite HMR on HTML/CSS/TS/JS, plus resolution, visibility, locale, transparency, event-injection and bridge-traffic controls.

The harness speaks the real bridge protocol, handshake included: it answers your document's `osfui.hello` with `ready`, then the locale catalog, then every mock state key, then opens events. So an F5 in the browser exercises exactly the boot path a reload in Starfield does — "works until you refresh" is a browser-side bug, not an in-game one.

Describe what your backend would do in `osfui.mock.ts` (or `.js`). The `defineMock` default export has four fields plus named `scenarios` that shallow-overlay them (`?scenario=<name>`, or the toolbar's Scenario select):

```ts
export default defineMock({
  // Replayed to your document on every greeting, under YOUR mod id:
  //   osfui.state.on('acme.mymod/telemetry', render)
  state: { telemetry: { fuel: 42, status: 'NOMINAL' } },
  locale: 'en',
  locales: { en: { 'panel.title': 'Telemetry' } },
  // Keyed by ENDPOINT NAME. A value may be plain JSON or an (async) function of
  // the request payload, and is what request() resolves with. A 'papyrus.<name>'
  // entry answers osfui.papyrus.request('<name>', …).
  requests: {
    'acme.mymod.getReadout': (payload) => ({ line: `sector ${payload.sector}` }),
    'papyrus.price': 1200,
  },
});
```

The mock enforces endpoint **kinds**, since a wrong kind otherwise only shows up against the real runtime: a `request` naming a built-in send endpoint is rejected `wrong-endpoint-kind`, a `send` naming an unhandled endpoint surfaces `unknown-endpoint`, and a scenario answering a `send` warns that the view sent it one-way. Platform endpoints (`close`, `setVisible`, `view.ready`, `log`, `menu.open`, `ping`, `game.get`, …) are answered for you.

For full control, export `install(ctx)`. It runs in the view page before your code and can layer handlers ahead of the scenario engine, push messages, and add toolbar controls:

```ts
export function install(ctx: MockContext) {
  ctx.onCommand((kind, name, payload, io) => {
    if (kind === 'request' && name === 'acme.mymod.commit') {
      io.resolve({ ok: true });          // or io.reject('busy', 'try again')
      return true;                       // handled; stop the chain
    }
  });
  ctx.registerTools([{ id: 'ping', kind: 'button', label: 'Push telemetry' }], () => {
    ctx.send({ kind: 'state', mod: 'acme.mymod', key: 'telemetry', value: { fuel: 7 } });
    ctx.send({ kind: 'event', name: 'acme.mymod.arrived', payload: { args: ['Jemison'] } });
  });
}
```

`ctx.send` takes a native→web envelope verbatim, so it also rehearses platform pushes (`{ kind: 'event', name: 'settings.changed', … }`, `{ kind: 'state', mod: 'osfui', key: 'settings', … }`); the toolbar's envelope injector takes the same shape. The browser reloads when the mock changes, and a plain `osfui.mock.json` fixture still works. The mock lives at the project root so it can never ship with the views.

Debug with the browser's own DevTools — the shared kit is the production one, so the failures you see are the failures the game reports. Every rejection, timeout, missing bridge and dropped send prints with an `[osfui]` prefix; `localStorage["osfui:trace"] = "1"` plus a reload logs every envelope both directions. Details: [troubleshooting.md](troubleshooting.md#debugging-your-own-view-for-authors).

`npm run check` flags remote URLs and browser transports the in-game host doesn't support.

## Iterate in Starfield

```bat
npm run dev:game -- --deploy "C:\path\to\MO2\mods"
```

The first run asks for MO2's `mods` directory and remembers it. It creates a child mod folder named after the project directory and places the generated `SFSE/` tree inside. The command keeps the browser harness running, deploys views and backend once at startup, and writes an expiring author-mode marker beside OSF UI's `config.json`.

Start Starfield while it runs. Author mode needs no player-config edit: loaded views reload after a sync, and F12 opens WebView2 DevTools on the focused menu. Author mode is the same switch as `devMode`, so your view's console output is also forwarded to `SFSE\Logs\OSF UI.log` (errors ERROR, warnings WARN, rest DEBUG) — useful for a repro you can't keep DevTools open through. Stopping the command removes the marker; the runtime ignores markers older than twelve hours.

After the first deployment, saves re-sync view assets only. Starfield holds the plugin, compiled scripts and native files open all session, so rewriting them mid-session fails — the command says so and leaves the deployed copies alone. Close the game and restart the command to deploy a Papyrus or backend change. Starting the command with the game already running behaves the same: views deploy, the rest is reported locked.

To remember paths, create the ignored `.osfui/local.json`:

```json
{
  "modsRoot": "C:\\path\\to\\MO2\\mods",
  "starfieldRoot": "D:\\SteamLibrary\\steamapps\\common\\Starfield",
  "spriggitCli": "D:\\Tools\\Spriggit\\Spriggit.CLI.exe"
}
```

Only `modsRoot` is normally needed; the others are Papyrus overrides for portable or nonstandard installs (standard Steam/CK locations and Spriggit on `PATH` are found automatically).

## Build and release

```bat
npm run check
npm run build
npm run package
```

For a Papyrus project, `build` first regenerates the ESM from the checked-in `spriggit/` source and compiles every `mod/Scripts/Source/**/*.psc`. It then copies the project's `mod/` Data-root tree into `dist/`, creates `dist/SFSE/Plugins/OSFUI/views/`, generates manifests from `osfui.config.ts|js`, and includes the public shared kit. Native DLLs, settings schemas and other normal mod files under `mod/` are included too. `package` rebuilds and writes a distributable zip under `release/`.

Declare `targetVersion` on your view in `osfui.config.*`. It's advisory — the runtime never gates on it — but it tells a player's Mods surface whether your view needs a newer OSF UI, and a view still declaring a 1.x target is flagged as written for the removed API rather than loading into a blank page.

Papyrus builds require:

- [Spriggit CLI](https://github.com/Mutagen-Modding/Spriggit/releases), on `PATH` or named by `spriggitCli`;
- Starfield Creation Kit's Papyrus compiler;
- CK's `Tools/ContentResources.zip`, whose `Scripts/Source` folder is extracted once into the ignored `.osfui/` cache.

The scaffold pins its compatible Spriggit translation version in the text source and OSF UI's matching `OSFUI.psc` under `tools/papyrus/`. `npm run doctor` verifies all of this and names the exact missing prerequisite. `check` and `doctor` warn on a missing or stale ESM/PEX; `build`, `package` and `dev:game` regenerate it rather than shipping a silently inert backend.

Set `modRoot` in `osfui.config.*` only if your Data-root source tree uses a different directory name.
