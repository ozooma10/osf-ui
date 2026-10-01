# Compatibility fixtures

Schemas and manifests copied from the six mod packages supplied by the user in
`OSF UI Compat Mods` on 2026-09-26. These are test inputs only, not deployed content.
DDC's schema is version 3; Somatic Camera SF and AEGIS are version 1. The Shop,
AISS Companion Log, and Starcade manifests target OSF UI 1.0 and omit
`manifestVersion`. Runtime AISS/Starcade key schemas are represented in the test.

The corresponding native DLLs and HUD files were inspected separately and are
not included. Fixture success does not prove their behavior in the game.

On 2026-09-30 the installed SomaticCameraSF DLL (logged version 1.0.0.4,
SHA-256 `FDE2B5D40925165F39CBC6DA09C0339B24323291450BBE7378C52BF53FE262D1`)
was checked directly: RVA `0x184846` loads `0x10008` before requesting the
bridge, and RVA `0x18488E` selects vtable offset `0x48` (SubscribeSettings).
`legacy_bridge_tests.cpp` covers that version request and settings callback
path. The appended 1.8 slot matches `sdk/OSFUI_API.h` at commit `bd89c384`.

`ssse-settings.json` was copied unchanged on 2026-10-01 from the installed
`X-2357's Settled Systems Stock Exchange/SFSE/Plugins/OSFUI/settings/x2357.ssse.json`.
SHA-256: `0510404c0584bc61d71b35c5b59457e018855062dd85e6da6bbab72ad811341b`.
Its `openExchange` setting defaults to F7. No competing modern schema is supplied.
Consumer PEX/JavaScript hashes, recovered call shapes and acceptance limits are
recorded in `docs/ssse-compatibility-validation.md`. The consumer scripts are not
redistributed; the optional JavaScript integration test reads the installed file.
