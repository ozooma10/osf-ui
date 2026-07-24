# OSF UI frontend

Source for the built-in OSF UI views. This directory builds to
**`../data/OSFUI/views/`**, which the plugin ships.

> ## Never hand-edit `data/OSFUI/views/`
>
> Everything under it is generated. Edits there are silently destroyed by the
> next `npm run build`, and CI fails the moment the two disagree. Edit
> `frontend/src/` instead.

---

## One-time setup

```bat
cd frontend
npm ci
```

Node 20+ is required to *build the frontend*. It is **not** a runtime dependency
— players never need it, and neither does `xmake build`, the native test suite,
or `xmake install`. Only `npm run build` and the CI frontend job use it.

## Commands

| Command | Does |
|---|---|
| `npm run dev` | Vite dev server with the mock bridge. Develop any view without launching Starfield. |
| `npm run build` | Generate `../data/OSFUI/views/`, then run the output gates. |
| `npm test` | Vitest: pure logic, protocol, components, and build-output gates. |
| `npm run typecheck` | `tsc --noEmit`. |
| `npm run check:dist` | Rebuild into a scratch dir and byte-compare against the committed output. Fails if stale. |
| `npm run verify` | `typecheck` → `build` → `test`. What to run before pushing. |

`npm run build` must be run and its output **committed** whenever anything under
`frontend/src/` changes. `check:dist` is the CI gate that enforces this.

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
      settings/     the Mods surface
      keybinds/     the input map
      handoff/      the always-warm first-load link surface (platform-private)
      benchmark/    the renderer workload lab
  harness/          DEV ONLY — mock bridge, fixtures, fixed-resolution stage
  scripts/          build orchestrator + output gates
  test/             vitest suites
```

## Development

```bat
npm run dev
```

Then pick a view from the harness index, or deep-link:

| View | URL |
|---|---|
| Harness index | `http://localhost:8080/` |
| Mods (settings) | `http://localhost:8080/?view=osfui/settings` |
| Keybinds | `http://localhost:8080/?view=osfui/keybinds` |

Query parameters:

| Param | Effect |
|---|---|
| `?view=<modId>/<viewName>` | which view to mount |
| `?res=off` | disable the fixed 1600×900 stage and render fluid |
| `?fixtures=1` | load the richer demo dataset (also togglable in the toolbar) |
| `?locale=<code>` | switch locale; `pseudo` expands strings to catch tight layouts and hardcoded text |
| `?schema=<url>` | load a settings schema from a URL instead of the fixtures |

You can also drag-and-drop a settings schema JSON or a `<modId>_<locale>.json`
catalog onto the page.

### The fixed-resolution stage

Views declare an initial 1600×900 size (`manifest.json`); the runtime resizes
them to the game output aspect once the swapchain is known. The harness renders a
1600×900 frame scaled by `min(w/1600, (h-30)/900)`, never upscaled beyond 1:1.
**Develop with the stage on** for the baseline, then use `?res=off` to verify
responsive behavior at other output sizes.

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

Every frame in both directions is `{ type, requestId?, payload }`. Web→native is
always `type: "ui.command"` with the command name **inside** the payload. The
authoritative type definitions are `sdk/osfui.d.ts`; `src/lib/protocol.ts`
re-exports them rather than restating them, so the two cannot drift.

Load order in a view's `index.html` is load-bearing and asserted by the build
gates: `shared/osfui.js` → `padnav.js` → `main.js`. The helper must decorate
`window.osfui` before the bundle reads it.

## Shipping bundle constraints

The production WebView2 backend loads views at
`https://osfui.local/<mod>/<view>/<entry>`. The built-in artifacts retain a
deliberately conservative, stable bundle shape enforced by
`scripts/verify-output.mjs` and `test/build.*`:

- **One classic IIFE bundle per view.** Stable `main.js` filenames and no code
  splitting keep the committed output and public asset paths deterministic.
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
3. `manifest.json`'s `id` **must** equal the folder name — native rejects a
   mismatch — and `entry` must stay at the view root so `../../shared/` resolves.
4. Register it in `scripts/config.mjs`'s `VIEWS` array with `mode: 'bundle'`.
   `expectedOutputs()` picks up its four files automatically.
5. `npm run build && npm test`, then commit the generated output.

Set `mode: 'verbatim'` instead if you are migrating an existing hand-written
view — it ships `main.legacy.js` untouched, letting you prove the pipeline
round-trips byte-identically before changing any behaviour.

## Adding a shared component

Put it in `src/ui/`. Style it **only** with existing `osf-*` classes from
`shared-kit/osfui.css` — no new class names, no CSS modules, no CSS-in-JS. If
padnav must be able to navigate it, reproduce the relevant DOM contract from
`COMPATIBILITY.md` §3 and add an assertion to `test/dom-contracts.test.tsx`.

## Packaging

Nothing in packaging is frontend-aware, by design:

- `xmake.lua` installs `data/(OSFUI/**)` recursively.
- The after_build rule redeploys `data/**` to the MO2 mod folder.
- `tools/package.ps1` mirrors `data/OSFUI/*` into the archive staging dir.

All three are glob-based, so new chunks, fonts, or files are picked up with no
packaging change. What they require in exchange is **stable filenames** — the
build emits no content hashes, because the MO2 redeploy overlays without pruning
and would accumulate orphans.

See `../docs/PACKAGING.md`.
