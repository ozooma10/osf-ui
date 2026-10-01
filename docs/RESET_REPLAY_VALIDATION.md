# Reset/replay fix validation - 2026-10-01

The pending-reset ordering defect is fixed locally on OSF UI `dev`, based on
`b002fc360ec4a0bcf9fbeb6c044a56e8498f077a`. The code/test changes are uncommitted.
OSF Settings remains clean and unchanged at `c0afdf0ade3ce6b6bb0c45875302c16551d09a22`.
No game/MO2 deployment, commit, push, or publication was performed.

## Change and regression

When a greeted document receives a pending `data.reset`, the adapter now clears
that document's delivery marker for each affected current value and marks the
value dirty. The same pump republishes fresh state after the reset; if loading
is suspended, dirty values survive until resume. This also handles fresh greeting
replay occurring before the reset pump. Existing locking and pre-hello behavior
are unchanged.

The maintained `legacy_papyrus_tests.cpp` exercises reset -> publish -> greeting
-> pump and pump-before-greeting, consecutive resets, repeated greetings/reopens,
multiple documents/mods, removed keys, no duplicate next-tick delivery, and
suspension/resume. The initial added regression failed against pre-fix production
objects; the final suite passes all 136 checks.

## Executed verification

- All 30 existing native UI suites compiled freshly from 53 unique translation
  units in isolated output and passed. The refined test fixture was recompiled
  before the final run. Production/test input hashes are preserved with the pair.
- All four JavaScript tests passed with zero skips, including the unchanged
  installed Stock Exchange script. Its DOM rendering and game VM are not run.
- The full UI DLL rebuilt and Papyrus compilation passed through the existing
  package command with game/mod deployment environment variables cleared.
- Both staging and a fresh extraction of the completed ZIP passed ownership,
  modern API 2.0, legacy bridge 1.0-1.8/vtable 1.8, and all 55 compiled Global
  Native signature checks. All 12 archive files match staging byte for byte.
- Current Settings packaging passed with production `test_harness=n`, all 23
  native suites passed, and a disposable reinstall preserved four simulated
  user/other-mod sentinels. Its 15-file archive matches its committed source
  manifest. Unchanged native/SWF build outputs were reused; Papyrus recompiled.

## Matched local artifacts

Preserved pair directory: `C:\Modding\Starfield\Artifacts\OSF UI Settings\20261001-replayfix`. `pair.json` records provenance and hashes;
`source-fix.patch`, validation receipts/logs, and matching private DLL/PDB pairs
are alongside the two public ZIPs. These candidates supersede the previous
UI/Settings package identities for testing this fix.

| Artifact | SHA-256 |
| --- | --- |
| `OSF-UI-v2.0.0-replayfix.zip` | `0B9F3461E6D21EEDDE63E90DD1820552C32E1000C3542631328EAEC785FFEFD0` |
| `OSFSettings-1.0.0-replayfix-pair.zip` | `5459F6A5F0517D9A4DF545BA6E1B6024C8D939BDC7F36AF267CD1B5C97C9FE26` |
| UI DLL | `6DAC1FFBE2B6A3E93862DD2BC14FE4F13A5297B130B1D72ED5BFEBD9C06FEFED` |
| UI PEX | `59143443C2147667CB45674E7E1346356BF0B2FFAFAD6AC21F7FFF81E57B856D` |
| UI browser host | `678C6F1F7303FA55C8D4030E83035AEF6536C36B6AC5C05E48FC89674BB7A928` |
| Settings DLL | `07080CD49491D1ABD13F9652D601B8E7FDDE4BE46F9F86D802532942A9BD9820` |

Build commands used:

```powershell
# UI package command clears automatic deployment paths internally.
./tools/package.ps1 -Version 2.0.0 -Tag replayfix -NoPdb
# Run separately from the unchanged Settings repository; staging is isolated.
./packaging/build-archive.ps1 -Label replayfix-pair
```

## Remaining release gate

In-game acceptance is unverified. Use the exact pair above and unchanged consumer
packages in a disposable profile to check opening/rebinding, real market data,
trades/chart replies, close/reopen, pending requests during load, and fresh
publication after loading another save. Also verify launcher handoff and actual
mod-manager upgrade/persistence. Automated VM/browser doubles and archive overlay
checks do not establish those outcomes. See the
[consumer acceptance checklist](ssse-compatibility-validation.md#disposable-profile-in-game-acceptance---unverified).
