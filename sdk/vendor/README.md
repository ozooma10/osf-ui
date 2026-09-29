# OSF Settings SDK (vendored)

Exact copies of the OSF Settings SDK at commit
[`619d65053ec650333d5dfdf37912fed2325e61af`](https://github.com/ozooma10/osf-settings-slim/commit/619d65053ec650333d5dfdf37912fed2325e61af)
(launcher completion contract, 2026-09-29):

| Header | ABI |
| --- | --- |
| `OSFSettings.h` | settings 1.0 |
| `OSFSettingsRegistry.h` | registry payloads for settings 1.0 |
| `OSFSettings_Providers.h` | providers 1.0 (required for legacy settings) |
| `OSFSettings_Diagnostics.h` | diagnostics 1.0 |
| `OSFSettings_Launcher.h` | launcher 1.0 (optional) |

Include as `"vendor/OSFSettings.h"`; `sdk/` is on the include path. Copy these files unchanged when updating. OSF Settings owns their implementation; OSF UI owns the legacy adapter.

## OSF UI 2.0 release pairing

The companion dependency is OSF Settings **1.0.0**. The source baseline selected
for release testing is
[`08bc609bad21ede7668a68a64c6663e5cab1296b`](https://github.com/ozooma10/osf-settings-slim/commit/08bc609bad21ede7668a68a64c6663e5cab1296b),
whose SDK matches these five headers byte for byte. This identifies the source
candidate, not a published Settings release or completed in-game acceptance.

Settings and diagnostics ABI 1.0 are required by the runtime; providers ABI 1.0
supports legacy settings. Launcher ABI 1.0 supplies the `OpenFn` request ID and
`Complete` after-close handoff used by launcher views. Launcher acquisition is
optional for direct view opens, but the companion release must provide it for the
advertised launcher behavior.

Earlier development DLLs also advertised product version 1.0.0 and ABI 1.0 with
different layouts. Those builds are unsupported: the version numbers alone cannot
identify a matching development pair. Use this source baseline during testing and
the companion release artifact at publication. Record both candidate checksums
for game acceptance; any replacement must preserve these interfaces or update the
pairing and repeat the affected checks.
