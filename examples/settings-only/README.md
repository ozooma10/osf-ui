# Example — settings-only (zero code)

The simplest OSF UI mod configuration: **one JSON file, no code**. Drop it in
and it renders a full settings card with typed, validated, persisted controls.

## 5-minute quickstart

1. Copy `yourname.mymod.json` to `Data/SFSE/Plugins/OSFUI/settings/<author>.<modname>.json`.
   The filename stem is your mod id — `"<author>.<modname>"`, lowercase
   `[a-z0-9-]` segments with exactly one dot, where `author` is your
   Nexus/GitHub handle (dotless ids are reserved for the platform). It must
   match the `"id"` field (and it's how load-order conflicts resolve, exactly
   like any other file in your mod).
2. Edit the `title`, `groups`, and `settings` to taste.
3. Launch the game, open the OSF UI overlay, and your card is in the left rail.

That's the whole loop. To iterate without launching Starfield, use the browser
harness in [`../../frontend/`](../../frontend/README.md) — run
`npm --prefix frontend run dev`, open
`http://127.0.0.1:5173/__osfui/?view=osfui%2Fsettings`, and drag this file
onto the page.

## Declaring a version

`"targetVersion"` names the OSF UI release you authored against. It never gates
loading and it is not a dependency: it only lets the Mods surface badge **needs
update** when the player's OSF UI is older than the release that first shipped
something you used (tabbed `pages`, `onPress`, a newer base type). Without it,
an older host degrades silently — an unrecognised type renders read-only at your
default — and the player has no idea why.

The mod API 2.0 break does not reach this file. A *view* targeting below `2.0`
is flagged in System Health because it calls helper members that no longer
exist; a settings schema executes nothing, so a 1.x schema still loads and
renders exactly as it did.

## What this file shows

| Feature | Where |
| --- | --- |
| Toggle / slider / stepper / dropdown / segmented / text / textarea / colour / key | throughout |
| `visibleWhen` / `enabledWhen` conditions (sibling-key predicates) | `hud.mode`, `hud.opacity`, `hud.scale` |
| Slider unit formatting (`format`) — store 0–1, show `%` | `hud.opacity` |
| `optionLabels` — stable stored values, human display strings | `hud.mode` |
| `presets` — author-shipped value sets, applied as one batch | top of file |
| `note` blocks (micro-markdown: `**bold**`, `*italic*`, `` `code` ``) | Advanced group |
| `action` buttons (fires a mod-namespaced bridge **request**) | `recalibrate` |
| `requires: "restart"` badge + aggregated banner | `backend` |
| Per-mod `accent` tint | top of file |

## Reading the values back (consumption)

The card above stores values; making them *do something* is the mod's job:

- **SFSE plugin (C++):** fetch the bridge (`sdk/OSFUI_API.h`) and subscribe to
  your mod's changes / read typed getters — see `docs/native-plugin-api.md`
  (settings mirror + change subscriptions).
- **Papyrus:** `OSFUI.GetInt("yourname.mymod", "hud.scale")` etc., plus change
  callbacks and hotkey delivery via `OSFUI.RegisterForSettingChanges` /
  `OSFUI.RegisterForHotkey` — see `docs/authoring-settings.md` "From Papyrus"
  and the shipped `Scripts/Source/OSFUI.psc`.
- **A view of your own** needs nothing from this file: subscribe to the whole
  registry as state and re-render from it. It replays on every fresh document,
  so there is no load-time fetch to write and nothing to re-request after F5:

  ```js
  osfui.state.on('osfui/settings', (data) => {
    const mine = data.mods.find((m) => m.id === 'yourname.mymod');
    if (mine) render(mine.values);
  });
  // A single committed value, post-clamp, as it lands:
  osfui.on('settings.changed', ({ mod, key, value }) => {
    if (mod === 'yourname.mymod') apply(key, value);
  });
  ```

- **Action buttons** (`yourname.mymod.recalibrate`) are a bridge **request**
  into your own namespace, so your plugin registers them with `RegisterRequest`
  and settles each one exactly once:

  ```cpp
  static void OnRecalibrate(const OSFUI::API::Request& a_req, void*) noexcept
  {
      if (Recalibrate()) a_req.Respond(R"({"message":"Recalibrated"})");  // "{}" = silent success
      else               a_req.Reject("recalibrate-failed", "No sensor data yet");
  }

  // once, after SFSE kPostLoad
  if (auto* bridge = OSFUI::API::RequestBridge()) {
      bridge->RegisterRequest("yourname.mymod.recalibrate", &OnRecalibrate, nullptr);
  }
  ```

  The button sends `{ mod, key }` and waits 5 seconds. `message` becomes a
  toast; a rejection becomes an error toast carrying your sentence; a handler
  that never settles rejects with `timeout`, which the surface renders as
  **No response from yourname.mymod**. Papyrus cannot serve an action row — it
  listens on the fixed `papyrus.send` / `papyrus.request` endpoints, not on
  named endpoints of its own — so an action button needs a native plugin.

## Injection safety

Every string in this file (labels, hints, note text) is rendered with
`textContent` / `createElement` — never `innerHTML`. Untrusted schema text can't
inject markup. Keep that guarantee if you fork the renderer.

## Localization

Authors keep writing English directly in the settings JSON. A community
translator can add `Data/SFSE/Plugins/OSFUI/l10n/<id>_<locale>.json` without changing the
original mod. See `l10n/yourname.mymod_de.json`: stable setting keys produce
addresses such as `settings.hud.enabled.label`; the authored English is used
for every missing translation. Give groups, presets, notes, and images an
optional `id` when translations should survive array reordering.
