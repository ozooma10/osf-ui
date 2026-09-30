# OSF UI 2.0 API contract

OSF UI 2.0 introduces a new API while retaining an adapter for the documented
OSF UI 1.6 compatibility baseline. Both entry points live in `OSFUI.dll`.
Product, native API, and browser protocol versions describe different contracts.

| Surface | Release contract | Consumer |
| --- | --- | --- |
| Plugin version | 2.0.0 | Installation and release identity |
| `OSFUI_RequestAPI` | Native API 1.0, `OSFUI::API::IUI` | New mods using [`OSFUI.h`](../sdk/OSFUI.h) |
| `OSFUI_RequestBridge` | Frozen native ABI 1.7 prefix plus the ABI 1.8 retained-state slot | Supported existing compiled mods |
| Modern browser bridge | Protocol 2.0, [`osfui.d.ts`](../sdk/osfui.d.ts) | New views using the shipped shared web helper |

The legacy export keeps its original signature and returns the legacy interface,
never `IUI`. Its adapter translates supported native calls, web messages, schemas,
and saved values into the current runtime and OSF Settings. See the
[compatibility scope and exclusions](compatibility-v1.md).

## Native SDK freeze

The current `OSFUI.h` defines the initial release contract for the new API at 1.0.
Existing virtual method order and signatures, callback signatures, payload layouts,
enum values, and documented behavior must remain compatible after this freeze.

- Compatible additions require a minor API version increase. Append new methods
  without moving existing slots and gate their use on the acquired API version.
- Changes to existing signatures or layouts require a new major interface. Do not
  return an incompatible object for a version request the caller already uses.
  Preserve previously supported interfaces through their own implementations or
  adapters if compatibility is promised.
- A plugin release version increase alone does not change the native ABI. The
  new API's versioning does not change the frozen legacy interface.

Earlier 2.0 development SDKs also advertised API 1.0 with different `IUI` layouts.
They are not supported release contracts. Rebuild their consumers against this
release SDK; the loader cannot distinguish them by the advertised API version.
Supported 1.6 mods using `OSFUI_RequestBridge` keep their original binaries.

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
