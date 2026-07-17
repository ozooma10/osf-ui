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

**0.5 — unstable.** Minor bumps may break views until it reaches 1.0. Don't do
version arithmetic — feature-detect against the `capabilities` array of the
`runtime.ready` handshake (append-only names; the shipped
`views/shared/osfui.js` helper wraps it):

```ts
const info = await osfui.ready;          // runtime.ready payload
if (!osfui.has("settings")) {
  console.warn("This OSF UI has no settings surface", info.version);
}
```

The version constant lives in [`src/core/Version.h`](../src/core/Version.h)
(`kBridgeProtocolVersion`) and is emitted by
[`src/runtime/MessageBridge.cpp`](../src/runtime/MessageBridge.cpp), which
also owns the capability list ([`src/runtime/Capabilities.h`](../src/runtime/Capabilities.h)).

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
