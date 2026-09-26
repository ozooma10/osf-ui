# OSF Settings SDK (vendored)

Exact copies of the sibling OSF Settings checkout's SDK at commit `72bc3195e4668a7cf528827fc06692fd0dd87311` (runtime-provider implementation, 2026-09-26):

| Header | ABI |
| --- | --- |
| `OSFSettings.h` | settings 1.0 (current pre-release contract) |
| `OSFSettingsRegistry.h` | registry payloads for settings 1.0 |
| `OSFSettings_Providers.h` | providers 1.0 (required for legacy settings) |
| `OSFSettings_Diagnostics.h` | diagnostics 1.0 |
| `OSFSettings_Launcher.h` | launcher 1.0 (optional) |

Include as `"vendor/OSFSettings.h"`; `sdk/` is on the include path. Copy these files unchanged when updating. OSF Settings owns their implementation; OSF UI owns the legacy adapter.
