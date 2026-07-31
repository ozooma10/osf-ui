# OSF UI view toolchain

The fastest authoring loop is a normal npm project. You do not need an OSF UI
source checkout, a hand-written manifest, or a custom Vite configuration.

## Create a view

```bat
npm create osfui@latest my-view
cd my-view
npm run doctor
npm run dev
```

The generator asks for a stable mod/view id, a menu or HUD surface, and a
Papyrus or native-plugin starting workflow. Projects are TypeScript with
strict checking and no UI framework dependency; hand-written plain `.js`
modules also build (`allowJs`). Generated source uses the production `src/views/<mod>/<view>/` shape and the
selected workflow adds its matching backend starter: Papyrus source or a native
SFSE/CommonLibSF plugin project. The Papyrus preset also includes Spriggit text
source for a Start Game Enabled quest and player alias, so it builds a real ESM
instead of leaving record setup as a manual Creation Kit exercise.

## Iterate in the browser

`npm run dev` opens the OSF UI harness. Saving HTML, CSS, TypeScript, or JavaScript
updates the view through Vite HMR. The harness provides the production shared
kit, injects the native bridge before application code, and offers resolution,
visibility, locale, transparency, event injection, and bridge-traffic controls.

The harness speaks the same bridge protocol the game does, including the
handshake: it answers your document's `osfui.hello` with `ready`, then the
locale catalog, then every mock state key, then opens events. That is
deliberate — an F5 in the browser exercises exactly the boot path a reload in
Starfield does, so "works until you refresh" is a bug you find in the browser
rather than in game.

Edit `osfui.mock.ts` or `osfui.mock.js` to describe what your backend would do.
The default export (`defineMock`) has four fields plus named `scenarios` that
shallow-overlay them (`?scenario=<name>`, or the toolbar's Scenario select):

```ts
export default defineMock({
  // Replayed to your document on every greeting, under YOUR mod id:
  //   osfui.state.on('acme.mymod/telemetry', render)
  state: { telemetry: { fuel: 42, status: 'NOMINAL' } },
  locale: 'en',
  locales: { en: { 'panel.title': 'Telemetry' } },
  // Keyed by ENDPOINT NAME. A value may be plain JSON or an (async) function
  // of the request payload, and is what request() resolves with. A
  // 'papyrus.<name>' entry answers osfui.papyrus.request('<name>', …).
  requests: {
    'acme.mymod.getReadout': (payload) => ({ line: `sector ${payload.sector}` }),
    'papyrus.price': 1200,
  },
});
```

The mock enforces endpoint **kinds**, because getting a kind wrong is a mistake
that only shows up against the real runtime otherwise: a `request` naming a
built-in send endpoint is rejected `wrong-endpoint-kind`, a `send` naming an
endpoint nothing handles is surfaced as `unknown-endpoint`, and a scenario that
answers a `send` warns that the view sent it one-way. Platform endpoints every
host provides (`close`, `setVisible`, `view.ready`, `log`, `menu.open`,
`ping`, `game.get`, …) are answered for you and need no mock entry.

For full control, export `install(ctx)`. It runs inside the view page before
your code and can layer handlers ahead of the scenario engine, push messages,
and add toolbar controls:

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

`ctx.send` takes a native→web envelope verbatim, so it is also how you rehearse
platform pushes your view reacts to (`{ kind: 'event', name: 'settings.changed', … }`,
`{ kind: 'state', mod: 'osfui', key: 'settings', … }`). The toolbar's envelope
injector takes the same shape. The browser reloads when the mock changes, and a
plain `osfui.mock.json` fixture keeps working. The mock lives at the project
root so it can never ship with the views.

Debugging is the browser's own DevTools, and the shared kit is the production
one, so the failures you see here are the failures the game reports: every
rejection, timeout, missing bridge and dropped send prints to the console with
an `[osfui]` prefix, and `localStorage["osfui:trace"] = "1"` plus a reload logs
every envelope in both directions. Details in
[troubleshooting.md](troubleshooting.md#debugging-your-own-view-for-authors).

Use `npm run check` to detect remote URLs and browser transports that the
in-game host does not support.

## Iterate in Starfield

```bat
npm run dev:game -- --deploy "C:\path\to\MO2\mods"
```

The first run asks for MO2's `mods` directory and remembers it locally. It
creates a child mod folder matching the project directory name, then places the
generated `SFSE/` tree inside it. You can also supply `--deploy` as shown above.
The command keeps the browser harness running, deploys this project's views and
backend once at startup, and writes an expiring author-mode marker beside OSF
UI's `config.json`.

Start Starfield while the command is running. Author mode is active without
editing player configuration: loaded views reload automatically after a sync,
and F12 opens WebView2 DevTools on the focused menu. Author mode is the same
switch as `devMode`, so your view's console output is also forwarded into
`SFSE\Logs\OSF UI.log` (errors at ERROR, warnings at WARN, everything else at
DEBUG) — useful for a repro you cannot keep DevTools open through. Stopping the
command removes the marker; the runtime ignores markers older than twelve hours
after an interrupted session.

After that first deployment, saves only re-sync the view assets. Starfield keeps
the plugin, the compiled scripts, and the native files open for the whole
session, so rewriting them mid-session fails; the command says so and leaves the
deployed copies alone. Close the game and restart the command to deploy a
Papyrus or backend change. Starting the command while the game already runs
works the same way: it deploys the views and warns that the rest is locked.

To remember the deployment directory, create the ignored local file
`.osfui/local.json`:

```json
{
  "modsRoot": "C:\\path\\to\\MO2\\mods",
  "starfieldRoot": "D:\\SteamLibrary\\steamapps\\common\\Starfield",
  "spriggitCli": "D:\\Tools\\Spriggit\\Spriggit.CLI.exe"
}
```

Only `modsRoot` is normally needed. The extra paths are Papyrus overrides for
portable or nonstandard installs; standard Steam/Creation Kit locations and
Spriggit on `PATH` are found automatically.

## Build and release

```bat
npm run check
npm run build
npm run package
```

For a generated Papyrus project, `build` first regenerates its ESM from the
checked-in `spriggit/` source and compiles every `mod/Scripts/Source/**/*.psc`.
It then copies the project's `mod/` Data-root tree into `dist/`,
then creates `dist/SFSE/Plugins/OSFUI/views/`, generates manifests from
`osfui.config.ts` or `osfui.config.js`, and includes the public shared kit.
Native DLLs, settings schemas, and other normal mod files under `mod/` are
included too. `package` rebuilds and writes a ready-to-distribute zip under
`release/`.

Declare `targetVersion` on your view in `osfui.config.*`. It is advisory — the
runtime never gates on it — but it is what tells a player's Mods surface
whether your view needs a newer OSF UI than they have, and a view still
declaring a 1.x target is flagged as written for the removed API rather than
loading into a blank page.

Papyrus builds require:

- [Spriggit CLI](https://github.com/Mutagen-Modding/Spriggit/releases), kept
  anywhere on `PATH` or named by `spriggitCli`;
- Starfield Creation Kit's Papyrus compiler; and
- Creation Kit's `Tools/ContentResources.zip`, whose `Scripts/Source` folder
  is extracted once into the ignored `.osfui/` cache.

The scaffold pins its compatible Spriggit translation version in the text
source and pins OSF UI's matching `OSFUI.psc` under `tools/papyrus/`.
`npm run doctor` verifies all of these pieces and fails with the exact missing
prerequisite. `check` and `doctor` still warn when an ESM or PEX is missing or
stale; `build`, `package`, and `dev:game` now fix that state automatically
instead of shipping a silently inert backend.

Set `modRoot` in `osfui.config.*` only when your Data-root source tree uses a
different directory name.
</content>
</invoke>
