# OSF UI SDK

Type definitions and tooling for building against the OSF UI bridge. This is the
seed of the frontend SDK — there is **no npm package or build step** yet;
everything here is hand-written and copied into a view or plugin project as
needed.

## Contents

- [`osfui.d.ts`](osfui.d.ts) — TypeScript definitions for **view authors**:
  `window.osfui`, the five native→web / two web→native envelopes, the platform
  send/request endpoint unions, the platform state keys and events, and the
  settings-schema shapes.
- [`OSFUI_API.h`](OSFUI_API.h) — the copyable C++ header for **SFSE plugin
  authors** (native bridge, C ABI 2.0). Consume it through the
  `OSFUI::API::Client` wrapper — it version-gates every call so a too-old
  host degrades to false/no-op instead of undefined behavior.
- [`OSFUI_JSON.h`](OSFUI_JSON.h) — optional header-only `nlohmann::json`
  parsing, response, state, and outbound-message conveniences. It compiles into
  the consuming plugin and does not change the dependency-free DLL ABI. See
  [docs/native-plugin-api.md](../docs/native-plugin-api.md).

## Bridge protocol version

**2.0 — stable.** Additive changes bump the minor; breaking changes bump the
major. Compatibility is advisory, not gated: declare the OSF UI version you
authored against as `targetVersion` (in your view manifest and/or settings
schema) and the Mods surface shows a "needs update" badge when the running host
is older.

2.0 is a breaking release in both directions, so the two ends fail differently:

- **Views** still load on a 2.0 host whatever they target, because the shipped
  helper is the only one there is. A manifest targeting anything below 2.0
  raises a `compat.legacy-view` card in System Health, since a 1.x view will
  render blank — every helper member it calls (`emit`, `call`, `action`,
  `viewReady`, `data.*`) was removed, and removed members fail loudly.
- **Plugins** do not load at all across the major. `OSFUI_RequestBridge` returns
  `nullptr` for an ABI major mismatch, and the refusal raises a
  `compat.legacy-api` card naming the offending DLL. See
  [docs/native-plugin-api.md §1](../docs/native-plugin-api.md#1-the-20-break)
  for why that is a hard break rather than a compatibility dispatcher.

The handshake is page-initiated: the document greets the bridge with
`osfui.hello` and the host answers `ready`, then replays state. The shared helper
does the greeting for you on every document, so first open, F5, dev hot-reload
and crash recovery are one path.

```ts
const info = await osfui.ready;          // the `ready` payload (RuntimeInfo)
console.log("running OSF UI", info.version, "bridge", info.bridgeVersion);
console.log("this document is", info.view, "of mod", info.mod);
```

`bridgeVersion` is informational — gate on nothing, declare `targetVersion`.

The version constants live in [`src/core/Version.h`](../src/core/Version.h)
(`kPluginVersion`, `kBridgeProtocolVersion`); the envelopes and dispatch are in
[`src/runtime/MessageBridge.cpp`](../src/runtime/MessageBridge.cpp). CI checks
that the headline above still names the version the code claims — the "docs say
0.1, code says 0.4" drift class is not allowed to recur.

## Validating your JSON files

JSON Schemas for the two author-facing file formats live in
[`docs/schema/`](../docs/schema/):

- `manifest.schema.json` — for `views/<modId>/<viewName>/manifest.json`
- `settings-schema.schema.json` — for `settings/<author>.<modname>.json`

Point your editor at them (e.g. VS Code `json.schemas`, or a top-level
`"$schema"` key) for autocomplete and validation while you author.

## See also

- [docs/authoring-views.md](../docs/authoring-views.md) — the full prose guide for views.
- [docs/native-plugin-api.md](../docs/native-plugin-api.md) — the C ABI guide for SFSE plugins.
- [docs/authoring-settings.md](../docs/authoring-settings.md) — settings schemas, and reading them from JS, C++ or Papyrus.
- [docs/mod-api-2.0-design.md](../docs/mod-api-2.0-design.md) — the four verbs, and why 2.0 looks like this.
- [docs/security-model.md](../docs/security-model.md) — what your view may and may not do.
