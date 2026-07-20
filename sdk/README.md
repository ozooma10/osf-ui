# OSF UI SDK

Type definitions and tooling for building views against the native bridge. This
is the seed of the frontend SDK — there is **no npm package or build step** yet;
everything here is hand-written and copied into a view project as needed.

## Contents

- [`osfui.d.ts`](osfui.d.ts) — TypeScript definitions for
  `window.osfui`, the message envelope, the `ui.command` whitelist, and the
  native→web message + settings-schema shapes (for **view authors**).
- [`OSFUI_API.h`](OSFUI_API.h) — the copyable C++ header for **SFSE plugin
  authors** (native bridge, C ABI 1.6). Consume it through the
  `OSFUI::API::Client` wrapper — it version-gates every call so a too-old
  host degrades to false/no-op instead of undefined behavior. See
  [docs/native-plugin-api.md](../docs/native-plugin-api.md).

## Bridge protocol version

**1.1 — stable.** Additive changes bump the minor; breaking changes bump the
major. Compatibility is advisory, not gated: declare the OSF UI version you
authored against as `targetVersion` (in your view manifest and/or settings
schema) and the Mods surface shows a "needs update" badge when the running
host is older. The host's own version arrives in the `runtime.ready`
handshake:

```ts
const info = await osfui.ready;          // runtime.ready payload
console.log("running OSF UI", info.version, "bridge", info.bridgeVersion);
```

The version constants live in [`src/core/Version.h`](../src/core/Version.h)
(`kPluginVersion`, `kBridgeProtocolVersion`); the handshake is emitted by
[`src/runtime/MessageBridge.cpp`](../src/runtime/MessageBridge.cpp).

## Validating your JSON files

JSON Schemas for the two author-facing file formats live in
[`docs/schema/`](../docs/schema/):

- `manifest.schema.json` — for `views/<modId>/<viewName>/manifest.json`
- `settings-schema.schema.json` — for `settings/<author>.<modname>.json`

Point your editor at them (e.g. VS Code `json.schemas`, or a top-level
`"$schema"` key) for autocomplete and validation while you author.

## See also

- [docs/authoring-views.md](../docs/authoring-views.md) — the full prose guide.
- [docs/security-model.md](../docs/security-model.md) — what your view may and may not do.
