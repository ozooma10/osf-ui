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
  authors** (native bridge, additive C ABI 1.9). Consume it through the
  `OSFUI::API::Client` wrapper — it version-gates every call so a too-old
  OSF UI runtime degrades to false/no-op instead of undefined behavior.
- [`OSFUI_JSON.h`](OSFUI_JSON.h) — optional header-only `nlohmann::json`
  parsing, response, state, and outbound-message conveniences. It compiles into
  the consuming plugin and does not change the dependency-free DLL ABI. See
  [docs/native-plugin-api.md](../docs/native-plugin-api.md).

## Web bridge protocol version

**2.0 — stable.** Additive changes bump the minor; breaking changes bump the
major. Declare the OSF UI version you authored against as `targetVersion` in
your manifest and settings schema. Newer targets receive a "needs update"
badge; explicitly pre-2.0 views use a guarded, frozen helper matching the API
they were authored against.

The web protocol made a breaking 2.0 cut, but the native ABI remains append-only.
ABI 1.8 appended retained `SetViewState`; ABI 1.9 appends strict
`RegisterSend` / `UnregisterSend`. Existing binaries keep the vtable prefix and
behavior they were compiled against. The frozen `RegisterCommand` still accepts
requests with request-id injection and automatic acknowledgement; new code uses
`RegisterSend` or `RegisterRequest` for an explicit endpoint kind.

The handshake is page-initiated: the document greets the bridge with
`osfui.hello` and the OSF UI runtime answers `ready`, then replays state. The shared helper
does the greeting for you on every document, so first open, F5, dev hot-reload
and crash recovery are one path.

```ts
const info = await osfui.ready;          // the `ready` payload (RuntimeInfo)
console.log("running OSF UI", info.version, "bridge", info.bridgeVersion);
console.log("this document is", info.view, "of mod", info.mod);
```

`bridgeVersion` is informational — gate on nothing, declare `targetVersion`.

The version constants live in [`src/Core/Version.h`](../src/Core/Version.h)
(`kOsfuiReleaseVersion`, `kBridgeProtocolVersion`); the envelopes and dispatch are in
[`src/Bridge/MessageBridge.cpp`](../src/Bridge/MessageBridge.cpp). CI checks
that the headline above still names the version the code claims — the "docs say
0.1, code says 0.4" drift class is not allowed to recur.

## Validating your JSON files

JSON Schemas for the two author-facing file formats live in
[`docs/schema/`](../docs/schema/):

- `manifest.schema.json` — for `views/<modId>/<viewName>/manifest.json`
- `settings-schema.schema.json` — for `settings/<modId>.json`

Point your editor at them (e.g. VS Code `json.schemas`, or a top-level
`"$schema"` key) for autocomplete and validation while you author.

## See also

- [docs/authoring-views.md](../docs/authoring-views.md) — the full prose guide for views.
- [docs/native-plugin-api.md](../docs/native-plugin-api.md) — the C ABI guide for SFSE plugins.
- [docs/authoring-settings.md](../docs/authoring-settings.md) — settings schemas, and reading them from JS, C++ or Papyrus.
- [docs/mod-api-2.0-design.md](../docs/mod-api-2.0-design.md) — the four verbs, and why 2.0 looks like this.
- [docs/security-model.md](../docs/security-model.md) — what your view may and may not do.
