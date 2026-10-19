# OSF Settings SDK (vendored)

Exact copies of the OSF Settings SDK at commit
[`ed635f25996308c6112a26890207ab0186ccb663`](https://github.com/ozooma10/osf-settings-slim/commit/ed635f25996308c6112a26890207ab0186ccb663)
(shared status codes and service clients, 2026-09-29):

| Header | ABI |
| --- | --- |
| `OSFSettings.h` | settings 1.0 |
| `OSFSettingsRegistry.h` | registry payloads for settings 1.0 |
| `OSFSettings_Providers.h` | providers 1.0 (required for legacy settings) |
| `OSFSettings_Diagnostics.h` | diagnostics 1.0 |
| `OSFSettings_Launcher.h` | launcher 1.0 (optional) |

Include as `"vendor/OSFSettings.h"`; `sdk/` is on the include path. Copy these files unchanged when updating. OSF Settings owns their implementation; OSF UI owns the legacy adapter.

## OSF UI 2.1 release pairing

The production artifact used for release testing is OSF Settings **1.1.2**, source
commit `6de2876bab663f6a515e99dedf7ac294bbb9942a`. Its five SDK headers match these
vendored files after line-ending normalization; the required ABI versions are
unchanged. The [2.1 release record](../../packaging/release-2.1.md) pins the exact
archive and acceptance evidence. A reused version number alone does not identify
the tested artifact, and local acceptance does not establish publication status.

### Historical 2.0 pairing

The companion dependency is OSF Settings **1.0.0**. The source baseline selected
for release testing is
[`a7ffa1860beab086414c02d1037b283c20031f29`](https://github.com/ozooma10/osf-settings-slim/commit/a7ffa1860beab086414c02d1037b283c20031f29),
whose SDK matches these five headers byte for byte. This identifies the source
candidate, not a published Settings release or completed in-game acceptance.

Settings and diagnostics ABI 1.0 are required by the runtime; providers ABI 1.0
supports legacy settings. Launcher ABI 1.0 supplies the `OpenFn` request ID and
`Complete` after-close handoff used by launcher views. Launcher acquisition is
optional for direct view opens, but the companion release must provide it for the
advertised launcher behavior.

Launcher results use the shared `OSFSettings::API::Status` codes, including
`UnknownLauncher` and `UnknownLaunchRequest`.

Earlier development DLLs also advertised product version 1.0.0 and ABI 1.0 with
different layouts. Those builds are unsupported: the version numbers alone cannot
identify a matching development pair. Use this source baseline during testing and
the companion release artifact at publication. Record both candidate checksums
for game acceptance; any replacement must preserve these interfaces or update the
pairing and repeat the affected checks.
