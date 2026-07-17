# Example — settings-only (zero code)

The simplest OSF UI mod configuration: **one JSON file, no code**. Drop it in
and it renders a full settings card with typed, validated, persisted controls.

## 5-minute quickstart

1. Copy `yourname.mymod.json` to `Data/OSFUI/settings/<author>.<modname>.json`.
   The filename stem is your mod id — `"<author>.<modname>"`, lowercase
   `[a-z0-9-]` segments with exactly one dot, where `author` is your
   Nexus/GitHub handle (dotless ids are reserved for the platform). It must
   match the `"id"` field (and it's how load-order conflicts resolve, exactly
   like any other file in your mod).
2. Edit the `title`, `groups`, and `settings` to taste.
3. Launch the game, open the OSF UI overlay, and your card is in the left rail.

That's the whole loop. To iterate without launching Starfield, use the browser
harness in [`../../devtools/harness/`](../../devtools/harness/) — drag this
file onto the page (or load `?schema=../../examples/settings-only/yourname.mymod.json`).

## What this file shows

| Feature | Where |
| --- | --- |
| Toggle / slider / stepper / dropdown / segmented / text / textarea / colour / key | throughout |
| `visibleWhen` / `enabledWhen` conditions (sibling-key predicates) | `hud.mode`, `hud.opacity`, `hud.scale` |
| Slider unit formatting (`format`) — store 0–1, show `%` | `hud.opacity` |
| `optionLabels` — stable stored values, human display strings | `hud.mode` |
| `presets` — author-shipped value sets, applied as one batch | top of file |
| `note` blocks (micro-markdown: `**bold**`, `*italic*`, `` `code` ``) | Advanced group |
| `action` buttons (fires a mod-namespaced bridge command) | `recalibrate` |
| `requires: "restart"` badge + aggregated banner | `backend` |
| Per-mod `accent` tint | top of file |

## Reading the values back (consumption)

The card above stores values; making them *do something* is the mod's job:

- **SFSE plugin (C++):** fetch the bridge (`sdk/OSFUI_API.h`) and subscribe to
  your mod's changes / read typed getters. (Native slice — see
  `docs/mcm-design.md` §8.)
- **Papyrus:** `OSFUI.GetInt("yourname.mymod", "hud.scale")` etc. (Native slice — §8.4.)
- **Action buttons** (`yourname.mymod.recalibrate`) are delivered to your plugin's
  registered command handler; reply with `{ type: "yourname.mymod.ack", payload: { key,
  ok, message } }` to resolve the button (otherwise it times out after 5s).

## Injection safety

Every string in this file (labels, hints, note text) is rendered with
`textContent` / `createElement` — never `innerHTML`. Untrusted schema text can't
inject markup. Keep that guarantee if you fork the renderer.

## Localization

Authors keep writing English directly in the settings JSON. A community
translator can add `Data/OSFUI/l10n/<id>_<locale>.json` without changing the
original mod. See `l10n/yourname.mymod_de.json`: stable setting keys produce
addresses such as `settings.hud.enabled.label`; the authored English is used
for every missing translation. Give groups, presets, notes, and images an
optional `id` when translations should survive array reordering.
