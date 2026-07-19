# OSF UI — panel dev harness (superseded)

**Use [`frontend/`](../../frontend/README.md) instead:**

```bat
npm --prefix frontend run dev
```

The built-in views are now built from `frontend/src/` (Vite + TypeScript +
Preact), and the Vite dev server is their harness: same mock bridge, same
fixed-resolution stage, same schema drag-drop, same locale/`pseudo` switch,
plus HMR and the real typed sources. `frontend/README.md` documents the
deep-link URLs and query parameters.

## Why it replaced these pages

These pages loaded the **shipped** assets from `data/OSFUI/views/`, which is
now generated build output — so they can only ever show you the last thing you
built, never what you are editing.

They also carried the structural flaw noted below: each page hand-copied its
view's `index.html` body markup, because the shipped page can't include the
mock script. Any change to a view's DOM had to be mirrored by hand, and
nothing caught it when it wasn't. **That hazard is gone** — the Vite harness
mounts each view from its own source, so there is exactly one copy of the
markup and no mirroring step.

## Status of the files here

`index.html`, `keybinds.html`, `osf.html`, `mockbridge.js` and `serve.cmd` are
still present and still run, as a fallback while the built-in views finish
migrating. They are frozen: fix them only if the Vite harness cannot yet do
what you need, and prefer fixing the Vite harness. Nothing here is packaged
(see [PACKAGING.md](../../docs/PACKAGING.md)).

Two stale claims in the previous version of this file, corrected for anyone
working on these pages:

- `osf.html` loads OSF Animation's view from that repo's
  `views/osf.animation/browser/` — **not** `views\osf\`. The view folder layout
  is two-level (`views/<modId>/<viewName>/`) and the mod id is `osf.animation`.
- It claimed l10n catalogs are fetched from the repo's `data/OSFUI/l10n/`.
  **That directory does not exist.** Catalogs come from a file you drop onto
  the page, or from `examples/settings-only/l10n/`. In game they load from
  `SFSE/Plugins/OSFUI/l10n/`, which is a path translation mods ship into — OSF
  UI itself ships no catalogs.
