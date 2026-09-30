# OSF UI 2.0 API contract

OSF UI 2.0 introduces a new API while retaining an adapter for the documented
OSF UI 1.6 compatibility baseline. Both entry points live in `OSFUI.dll`.
Product, native API, and browser protocol versions describe different contracts.

| Surface | Release contract | Consumer |
| --- | --- | --- |
| Plugin version | 2.0.0 | Installation and release identity |
| `OSFUI_RequestAPI` | Native API 2.0 (`0x00020000`), `OSFUI::API::IUI` | New mods using [`OSFUI.h`](../sdk/OSFUI.h) |
| `OSFUI_RequestBridge` | Frozen native ABI 1.7 prefix plus the ABI 1.8 retained-state slot | Supported existing compiled mods |
| Modern browser bridge | Protocol 2 (`protocolVersion: 2`), [`osfui.d.ts`](../sdk/osfui.d.ts) | New views using the shipped shared web helper |

The legacy export keeps its original signature and returns the legacy interface,
never `IUI`. Its adapter translates supported native calls, web messages, schemas,
and saved values into the current runtime and OSF Settings. See the
[compatibility scope and exclusions](compatibility-v1.md).

## Native SDK versioning

OSF UI 2.0 is not yet released. The current `OSFUI.h` defines the intended initial
release contract for the new API at 2.0; development-only names can change without
compatibility aliases. At release, virtual method order and signatures, callback
signatures, payload layouts, enum values, and documented behavior become frozen.
The following rules apply after that release:

- Compatible additions require a minor API version increase. Append new methods
  without moving existing slots and gate their use on the acquired API version.
- Changes to existing signatures or layouts require a new major interface. Do not
  return an incompatible object for a version request the caller already uses.
  Preserve previously supported interfaces through their own implementations or
  adapters if compatibility is promised.
- A plugin release version increase alone does not change the native ABI. The
  new API's versioning does not change the frozen legacy interface.

Earlier 2.0 development SDKs advertised API 1.0 with different `IUI` layouts.
API major 1 is reserved for those unsupported layouts and must not be reused.
The release export rejects API 1.x requests with `nullptr` and a zero output
version. Release consumers request API 2.0, so development DLLs implementing
API 1.0 reject them. The SDK also checks the returned ABI before casting the
interface; failed acquisition leaves `Client` detached. Update both the installed
OSF UI DLL and development consumers rebuilt against this release SDK.
Supported 1.6 mods using `OSFUI_RequestBridge` keep their original binaries.

## Browser protocol identity

The ready envelope identifies the browser wire protocol with the integer
`payload.protocolVersion: 2`. `payload.version` is the product release string,
such as `"2.0.0"`; it is not a protocol or native ABI compatibility check.
The development-only `bridgeVersion: "2.0"` field is removed without an alias.
The complete payload is:

```json
{"game":"Starfield","plugin":"OSF UI","version":"2.0.0","protocolVersion":2,"mod":"acme","view":"acme/panel"}
```

This browser version is independent of the private WebView2 host IPC protocol.

## Native initialization and readiness

The modern native API exposes `IsReady()` for global bridge availability and has
no readiness callbacks. A document reload can leave `IsReady()` true, so it does
not report individual view initialization, reloads, or first paint.

Register send/request handlers and publish `SetState` values without waiting for
readiness. Retained state is replayed automatically when a page connects after
creation or reload. Pages that need newly computed data can call a registered
request endpoint when they initialize. The browser ready/state/event handshake
and browser recovery are independent of native readiness callbacks.

Development consumers using native readiness callbacks must remove those
registrations and rebuild against the release SDK. The frozen legacy
`SetReadyCallback` slot remains available through `OSFUI_RequestBridge`; its
behavior and lifetime guarantees are documented under
[legacy native calls](compatibility-v1.md#views-and-native-calls).

## Companion dependency and consumers

Use the [OSF Settings 1.0.0 release pairing and SDK provenance](../sdk/vendor/README.md).
Its settings, diagnostics, providers, and launcher interfaces are separate from
the OSF UI API. Old Settings development builds are not substitutes for the
selected companion build even if they report the same version numbers.

The in-repository modern consumer is the
[settings-view example](../examples/settings-view/README.md). It includes the SDK
directly from this repository and must be rebuilt with the release headers.
Authors of other 2.0 development consumers must update their SDK copies and rebuild
their DLLs. A mod choosing to port from the old API follows
[the migration guide](../MIGRATION.md); the compatibility adapter does not require
that migration.

Legacy acceptance uses the original, unmodified packages listed in the
[compatibility baseline](compatibility-v1.md). Record the consumer and framework
versions/checksums and exercise the documented game scenarios. Successful native
tests and a rebuilt modern example do not establish that game acceptance.
