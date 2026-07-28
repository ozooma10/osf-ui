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

Node 20+ is required to *build the frontend*. It is **not** a runtime dependency
— players never need it. `npm run build`, `xmake build`, `xmake install`, and
release packaging use it; the native test suite does not.

## Commands

| Command | Does |
|---|---|
| `npm run dev` | Vite dev server with the mock bridge. Develop any view without launching Starfield. |
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
| `?res=fixed\|fill\|off` | stage mode: the 1600×900 frame (default), the same scale widened to fill the window, or no stage at all |
| `?fixtures=1` | load the richer demo dataset (also togglable in the toolbar) |
| `?locale=<code>` | switch locale; `pseudo` expands strings to catch tight layouts and hardcoded text |
| `?schema=<url>` | load a settings schema from a URL instead of the fixtures |
| `?health=<name>` | pin the System Health scenario pushed as `diagnostics.data` (also cycled by the toolbar "Health" button) |

### System Health scenarios

The Health destination renders from a `diagnostics.data` snapshot, so it needs no
broken game to exercise — the harness pushes a canned one. Scenarios live in
`harness/fixtures/diagnostics.ts`:

| `?health=` | What it shows |
|---|---|
| `clean` | nominal summary, no cards (the default) |
| `warnings` | warnings only |
| `errors` | an active error alongside a warning, with a degraded `system` block |
| `mixed` | both severities plus one resolved card |
| `resolved` | nominal summary but a non-empty history |
| `catalog` | **one card per known code, plus an unrecognised one** — the whole copy table on one page |

Use `catalog` to proof-read the diagnostic copy: every title, impact/next line and
action row the shell can emit, side by side. Add `&locale=pseudo` to check none of
it is hardcoded or overflowing. Adding a code to `COPY` in
`src/lib/settings/diagnostics.ts` without adding it to `catalog` means that card
has never been looked at.

You can also drag-and-drop a settings schema JSON or a `<modId>_<locale>.json`
catalog onto the page.

### The stage

Views declare an initial 1600×900 size (`manifest.json`); the runtime resizes
them to the game output aspect once the swapchain is known. The toolbar button
cycles three ways of modelling that, also settable with `?res=`:

| Mode | What it renders |
|---|---|
| `fixed` | the literal 1600×900 frame, letterboxed, scaled by `min(w/1600, (h-30)/900)` |
| `fill` | 900 reference rows tall, widened to the window's aspect and scaled by `(h-30)/900`, so the stage fills the window at the in-game text size |
| `off` | no stage: the view reflows to the raw browser window, unscaled |

Neither staged mode caps the scale at 1:1 — filling a 1080p window at 1.2× *is*
the in-game text size.

**Develop in `fixed` for the baseline**, use `fill` to see the view at your
window's aspect the way the game resizes it, and `off` to inspect raw overflow.

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
3. `manifest.json`'s `id` **must** equal the folder name — native rejects a
   mismatch — and `entry` must stay at the view root so `../../shared/` resolves.
4. Register it in `scripts/config.mjs`'s `VIEWS` array with `mode: 'bundle'`.
   `expectedOutputs()` picks up its four files automatically.
5. `npm run build && npm test`, then commit the source changes.

Set `mode: 'verbatim'` instead if you are migrating an existing hand-written
view — it ships `main.legacy.js` untouched, letting you prove the pipeline
round-trips byte-identically before changing any behaviour.

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
