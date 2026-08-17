# OSF UI frontend

Source for the built-in OSF UI views. This directory builds to
the ignored **`../build/frontend/views/`** artifact, which xmake ships.

> ## Never hand-edit `build/frontend/views/`
>
> Everything under it is disposable and replaced by the next build. Edit
> `frontend/src/`; generated bundles are not source-controlled.

---

## One-time setup

```bat
cd frontend
npm ci
```

Node 20+ is required to *build the frontend*. It is **not** a player dependency
— players never need it. `npm run build`, `xmake build`, `xmake install`, and
release packaging use it; the native test suite does not.

## Commands

| Command | Does |
|---|---|
| `npm run dev` | `osfui dev` — the authoring harness (same one third-party mods use) with the full mock bridge. Develop any view without launching Starfield. |
| `npm run build` | Generate `../build/frontend/views/`, then run the output gates. |
| `npm test` | Vitest: pure logic, protocol, components, and build-output gates. |
| `npm run typecheck` | `tsc --noEmit`. |
| `npm run verify` | `typecheck` → `build` → `test`. What to run before pushing. |

Commit source changes only; CI builds and validates a fresh ignored artifact.

## Layout

```
frontend/
  src/
    shared-kit/     FROZEN public contract, shipped verbatim (see COMPATIBILITY.md)
      osfui.js        the bridge helper third-party views link by exact path
      osfui.css       the --osf-* / .osf-* design-token kit
    legacy/
      padnav.js     spatial gamepad navigation, shipped verbatim (see COMPATIBILITY.md)
    lib/            pure typed modules — no DOM, no globals, no import-time side effects
      protocol.ts     re-exports sdk/osfui.d.ts; envelope encode/parse
      bridge.ts       typed façade over the window.osfui helper
      settings/       normalisation, conditions, filtering, conflicts, rail model
      keybinds/       canonical key names, model building, conflict detection
    ui/             shared Preact components (styled only with kit classes)
    views/osfui/
      settings/     the Mod Settings view
      keybinds/     the Keybindings view
  devmock/          DEV ONLY — the mock bridge + fixtures (installed by osfui.mock.ts)
  osfui.config.ts   the built-ins as an @osfui/cli project (what `osfui dev` serves)
  osfui.mock.ts     mock module: installs devmock/ + registers the toolbar tools
  scripts/          build orchestrator + output gates + builtin-dev-plugin (dev shims)
  test/             vitest suites
```

## Development

```bat
npm run dev
```

This is `osfui dev` on this directory's `osfui.config.ts` — the same authoring
harness third-party mods get, serving the built-ins through their real
`index.html` → shared-kit → padnav boot contract in an iframe. The rich mock
(`devmock/`, installed by `osfui.mock.ts`) registers the extra toolbar tools:
Reset values, Sample views, System Health scenarios, and Hotkey/LB/RB.

Pick a view from the toolbar's view select, or deep-link:

| View | URL |
|---|---|
| Harness | `http://127.0.0.1:5173/__osfui/` |
| Mod Settings | `http://127.0.0.1:5173/__osfui/?view=osfui%2Fsettings` |
| Keybindings | `http://127.0.0.1:5173/__osfui/?view=osfui%2Fkeybinds` |

Query parameters (`view` and `res` are the harness page's own; everything else
— and the `#hash` — is forwarded to the view, so mock params and `#mod=` deep
links keep working):

| Param | Effect |
|---|---|
| `?view=<modId>/<viewName>` | which view the harness shows |
| `?res=fill\|off` | stage mode: the game-true 900-row frame widened to the pane (default), or no stage at all |
| `?fixtures=1` | load the richer demo dataset (also the "Sample views" tool) |
| `?locale=<code>` | switch locale; `pseudo` expands strings to catch tight layouts and hardcoded text |
| `?schema=<url>` | load a settings schema from a URL instead of the fixtures |
| `?health=<name>` | select the local System Health snapshot (also the "Health" cycle tool) |

You can also drag-and-drop a settings schema JSON or a `<modId>_<locale>.json`
catalog onto the page.

### System Health scenarios

System Health renders the `osfui/diagnostics` state snapshot, so it needs no broken game to exercise. The harness scenarios in `devmock/fixtures/health.ts` are `clean`, `warnings`, `errors`, `mixed`, `resolved`, and the full copy catalog. They exercise only the local display and recovery surface; the mock exposes no report upload, submission, log-folder, or external issue-opening endpoint.

### The stage

Views declare an initial 1600×900 size (`manifest.json`); the OSF UI runtime resizes
them to the game output aspect once the swapchain is known. The toolbar button
cycles two ways of modelling that, also settable with `?res=`:

| Mode | What it renders |
|---|---|
| `fill` | 900 reference rows tall, widened to the pane's aspect and scaled by `paneHeight/900`, so the stage fills the pane at the in-game text size |
| `off` | no stage: the view reflows to the raw pane, unscaled |

`fill` does not cap the scale at 1:1 — filling a 1080p window at 1.2× *is* the
in-game text size.

**Develop in `fill`**: it is what the OSF UI runtime actually does to a view, so a
layout that holds up as you resize the window holds up at any game output
aspect. Switch to `off` to inspect raw overflow or measure in DevTools without
the scale transform in the way — it is a debugging mode, not a preview.

A third mode used to letterbox the literal 1600×900 box (`?res=fixed`). It was
removed: at a 16:9 window it is `fill` with bars, and at any other aspect it
shows a composition the game never produces.

## Native bridge architecture

```
  view (this bundle)
        │  window.osfui.send/request/on        ← src/lib/bridge.ts (typed façade)
        ▼
  shared/osfui.js  (frozen helper: correlation, timeouts, i18n, ready handshake)
        │  window.osfui.postMessage(json)
        ▼
  native MessageBridge  ──►  Runtime / SettingsStore / HotkeyService
        │  window.osfui.onMessage(json)        ← OWNED by the helper; never assign it
        ▼
  osfui.on(type, fn) subscribers
```

Bridge protocol 2.0 keeps routing beside the payload. Web→native frames are
`{ kind:"send", name, payload }` or
`{ kind:"request", name, id, payload }`. Native→web frames use
`kind:"ready"`, `"state"`, `"event"`, `"reply"`, or `"error"`; their routing
fields likewise sit beside the payload. The legacy `type`, `ui.command`, and
`requestId` envelope belongs only to the guarded 1.x compatibility façade.
The authoritative type definitions are `sdk/osfui.d.ts`;
`src/lib/protocol.ts` re-exports them rather than restating them, so the two
cannot drift. See [the terminology glossary](../docs/terminology.md) for the
readiness and component boundaries.

Load order in a view's `index.html` is load-bearing and asserted by the build
gates: `shared/osfui.js` → `padnav.js` → `main.js`. The helper must decorate
`window.osfui` before the bundle reads it.

## Shipping bundle constraints

The production web renderer asks the out-of-process browser host to navigate view documents to
`https://osfui.local/<modId>/<viewName>/<entry>`. The built-in artifacts retain a
deliberately conservative, stable bundle shape enforced by
`scripts/verify-output.mjs` and `test/build.*`:

- **One classic IIFE bundle per view.** Stable `main.js` filenames and no code
  splitting keep installed output and public asset paths deterministic.
- **ES2020 target.** This remains the project's chosen compatibility target.
- **No remote dependencies in built-ins.** No
  webfonts (all three `--osf-font-*` stacks resolve to Windows system faces), no
  CDN, no `fetch` to a remote host. The build fails on `@font-face` or a remote
  `url()`. This is a content gate, not a browser-level network sandbox; see
  `docs/security-model.md`.

## Adding a new view

1. `mkdir frontend/src/views/<modId>/<viewName>/` with `index.html`,
   `manifest.json`, `main.tsx`, `style.css`.
2. Copy an existing `index.html` shell verbatim. The three script tags and two
   stylesheet links, in that order, are asserted by the build gates.
3. The path is the identity: `<modId>` and `<viewName>` come from the two folder
   names and form `<modId>/<viewName>`. Do not add a manifest `id`; legacy `id`
   fields are ignored. `entry` must stay at the view root so `../../shared/`
   resolves.
4. `npm run build && npm test`, then commit the source changes. The build
   discovers the manifest and expects the view's four output files automatically.

## Adding a shared component

Put it in `src/ui/`. Style it **only** with existing `osf-*` classes from
`shared-kit/osfui.css` — no new class names, no CSS modules, no CSS-in-JS. If
padnav must be able to navigate it, reproduce the relevant DOM contract from
`COMPATIBILITY.md` §3 and add an assertion to `test/dom-contracts.test.tsx`.

## Packaging

Build and packaging share one generated artifact:

- `xmake build` generates `build/frontend/views/` and redeploys it to MO2.
- `xmake install` regenerates and stages the same tree.
- `tools/package.ps1` runs `npm ci`, then uses that install path.

The generated tree is copied recursively, so new chunks, fonts, or files need no
packaging change. It retains **stable filenames** and emits no content hashes,
keeping installed paths deterministic.

See `../docs/PACKAGING.md`.
