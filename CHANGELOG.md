# Changelog

## 1.0.0 — 2026-07-17

Initial release.

### Highlights

- **HTML/CSS/JS views over Starfield** - an SFSE/CommonLibSF plugin that renders web UI in game via the Ultralight engine. Inspired by Prisma UI.
- **Mods surface** - press **F10** (rebindable in game) to open the unified Mods menu, where OSF UI and content mods expose their settings.
- **Keybinds view** - a visual keyboard map with inline rebinding and conflict badges.

### For view authors

- View packages under `views/<modId>/<viewName>/` with a `manifest.json`;
  multiple views can load and composite together (layer order via `zorder`).
- JS bridge `window.osfui`, **protocol 0.5**: request envelope with
  `requestId` correlation, uniform `ui.result` outcomes, capability flags,
  and raw gamepad events (experimental).
- Declarative **settings schemas** with persistence under
  `Documents\My Games\Starfield\OSFUI\settings\`, versioning + migration,
  input contexts, and unbound-key support.
- Shared UI kit under the `--osf-*` CSS namespace, plus TypeScript
  definitions (`sdk/osfui.d.ts`).
- Developer loop: `devMode` verbose logging and first-frame PNG dump,
  **F11** in-place view reload, schema hot-reload, and a browser dev harness.

### For plugin authors

- Native C++ API (`sdk/OSFUI_API.h`): register shipped views at runtime and
  handle commands from your views.

### Engine integration

- Views open as real engine menus: proper menu-stack admission, game pause
  while the overlay is open, and a hardware cursor.
- Shipped `OSFUI/config.json` is the developer/boot file; user-facing
  settings live in the in-game menu and survive updates.
- The default build has zero Ultralight footprint; the real renderer is an
  opt-in build (`xmake f --with_ultralight=true`).
