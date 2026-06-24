# PrismaUI SF SDK

Type definitions and tooling for building views against the native bridge. This
is the seed of the frontend SDK — there is **no npm package or build step** yet;
everything here is hand-written and copied into a view project as needed.

## Contents

- [`prismaui-sf.d.ts`](prismaui-sf.d.ts) — TypeScript definitions for
  `window.prisma`, the message envelope, the `ui.command` whitelist, and the
  native→web message + settings-schema shapes (for **view authors**).
- [`PrismaUI_API.h`](PrismaUI_API.h) — the public C++ consumer API for **other
  SFSE plugins** to drive views (PrismaUI-compatible: `RequestPluginAPI` →
  `CreateView`/`Invoke`/`RegisterJSListener`/…). See
  [docs/consumer-api.md](../docs/consumer-api.md) and the
  [example consumer](../examples/consumer/).

## Bridge protocol version

**0.1 — unstable.** Minor bumps may break views until it reaches 1.0. Detect it
at runtime from the `runtime.ready` handshake and refuse/degrade on a mismatch:

```ts
window.prisma.onMessage = (json) => {
  const msg = JSON.parse(json);
  if (msg.type === "runtime.ready" && !msg.payload.bridgeVersion?.startsWith("0.")) {
    console.warn("Unsupported PrismaUI SF bridge", msg.payload.bridgeVersion);
  }
};
```

The version constant lives in [`src/core/Version.h`](../src/core/Version.h)
(`kBridgeProtocolVersion`) and is emitted by
[`src/runtime/MessageBridge.cpp`](../src/runtime/MessageBridge.cpp).

## Validating your JSON files

JSON Schemas for the two author-facing file formats live in
[`docs/schema/`](../docs/schema/):

- `manifest.schema.json` — for `views/<id>/manifest.json`
- `settings-schema.schema.json` — for `settings/<id>.json`

Point your editor at them (e.g. VS Code `json.schemas`, or a top-level
`"$schema"` key) for autocomplete and validation while you author.

## See also

- [docs/authoring-views.md](../docs/authoring-views.md) — the full prose guide.
- [docs/security-model.md](../docs/security-model.md) — what your view may and may not do.
