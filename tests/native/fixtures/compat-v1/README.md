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
