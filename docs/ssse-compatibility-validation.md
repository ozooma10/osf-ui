# Stock Exchange compatibility validation — 2026-10-01

Status: automated compatibility/build/package checks pass. **In-game acceptance is
unverified.** No game profile was launched, no live installation was deployed, and
Stock Exchange was not edited or rebuilt. No commit, push or publication was made.

Current candidate and follow-up fix: [RESET_REPLAY_VALIDATION.md](RESET_REPLAY_VALIDATION.md)
records the corrected reset/replay behavior, fresh test results, and exact matched
UI/Settings package hashes. Package identities and logs below describe the
earlier adapter implementation and are historical; use the new record for this
candidate. In-game acceptance remains unverified.

## Evidence and scope

The OSF UI checkout was clean on branch `dev` before this work. The applicable
workspace instructions were `C:/Modding/Starfield/AGENTS.md`; no additional
repository/subdirectory runbook was present. All 21 installed consumer files were
hashed and rechecked unchanged. Audit scratch files/logs are under `build/ssse-audit`.

The consumer is `C:/Modding/Starfield/MO2/mods/X-2357's Settled Systems Stock Exchange`.
Its original settings file and legacy view manifest identify `x2357.ssse`,
`x2357.ssse/exchange`, and `openExchange` (default F7). This remains the same setting
with the same legacy values path. The existing Settings translation is sufficient;
it required no new schema or provider for this mod.

| Installed input | SHA-256 |
| --- | --- |
| `Scripts/X2357SSSE_View.pex` | `0574f5c855fa73ae411ab17870276b3514dc6ea46bdfbe48b186bf145a281f22` |
| `SFSE/Plugins/OSFUI/views/x2357.ssse/exchange/main.js` | `39ed3effbb563a1bc89b4c827cbd770a55f1f1b94ee57784b1217b5bbf085d19` |
| `SFSE/Plugins/OSFUI/settings/x2357.ssse.json` | `0510404c0584bc61d71b35c5b59457e018855062dd85e6da6bbab72ad811341b` |
| `x2357ssse.esm` | `d0ef840b80728a3958ef29213762c01214e23d514f801581d6cf180cfac21a9d` |

Exact native signatures/behavior came from local `v1.6.0` files
`data/Scripts/Source/OSFUI.psc`, `src/api/PapyrusApi.cpp` and
`src/runtime/Runtime.cpp`. The installed PEX was disassembled and decompiled into
audit scratch using [Champollion v1.3.2](https://github.com/Orvid/Champollion/releases/tag/v1.3.2).
No decompiled consumer code is shipped. Its `Arm()` unregisters saved tokens,
registers the three listeners, and publishes. Both quest initialization and the
remote `Actor.OnPlayerLoadGame` event call `Arm()`. `OnHotkey(mod, key)` calls
`OSFUI.OpenMenu(ViewId)`; the page deliberately has no F7 handler.

The current backend lacked these bindings/handlers. The retained legacy helper
also lacked a modern `kind: state` translation, which would have prevented typed
state delivery even after restoring native setters.

## Restored contracts

All functions remain Global Native methods of `OSFUI`. Full declarations are in
`data/Scripts/Source/OSFUI.psc` and are compiled into the packaged `OSFUI.pex`.

| Family | Arguments in order | Return/delivery |
| --- | --- | --- |
| `SetViewBool/Int/Float/String` | `(string mod, string key, T value)` | void; retained typed state |
| `SetViewBools/Ints/Floats/Strings/Forms` | `(string mod, string key, T[] values)` | void; complete array replacement |
| `ListenForViewActions` | `(ScriptObject receiver, string mod)` | int token; `OnOSFUIViewAction(string, string[])` |
| `ListenForViewRequests` | `(ScriptObject receiver, string mod)` | int token; `OnOSFUIViewRequest(string, string[], string replyToken)` |
| `ReplyViewBool/Int/Float/String` | `(string replyToken, T value)` | bool; `papyrus.result { value }` |
| `ReplyViewBools/Ints/Floats/Strings/Forms` | `(string replyToken, T[] values)` | bool; same typed envelope |
| `RejectViewRequest` | `(string replyToken, string code, string message = "")` | bool; correlated error |
| `RegisterForHotkey` | `(ScriptObject receiver, string fn, string mod, string key = "")` | int token; `fn(string mod, string key)` |
| `Unregister` | `(int token)` | bool; false for stale/invalid tokens |

Static listeners take a script name in place of the instance. The earlier
`RegisterForViewActions[Args][Static]` variants retain their explicit callback
name and scalar/list shape. Request listeners are first-wins per mod. Typed form
values are captured as IDs and serialized on the runtime tick, preserving null
array slots and optional name/editorId metadata.

Real Stock Exchange shapes covered:

- State: `market` is float[22]; `prices/prev/open/high/low/base/change/held/cost`
  are float arrays; `stableIds/sectors/news/optPositions/optHistory/lastTrade`
  use typed integer arrays; `newsMag` uses floats.
- Actions: `buy(index, qty)`, `sell(index, qty)`, `refresh`, and
  `buyOption(index, call, strike, expiry, qty, expectedPremium)`. The original
  JavaScript's option-sale argument reorder is exercised unchanged.
- Requests: `candles(index, count)`, `intraday(index)`,
  `closes(index, count, stride)`, `optionQuote(index, call, strike, expiry)`,
  `optionPositions(offset)` and rejected requests. Successful replies retain
  their scalar or array type under `payload.value`.

The v1.6 implementation's default request deadline is 10 seconds; its helper's
15-second timer does not extend that backend deadline. The installed consumer
explicitly requests 60 seconds for chart walks. The adapted helper now forwards
that budget privately to this adapter (maximum 60 seconds). Modern APIs keep
their existing default. This is a deliberate timeout extension, not an assertion
that v1.6 already honored the consumer's larger timer.

The registry uses weak VM handles, case-insensitive matching and non-reused
registration tokens. Close keeps session listeners/state but cancels that view's
requests. Navigation discards old correlations before creating a new document.
Save replacement suspends calls, success clears registrations/state/requests,
failure resumes the old session, and the consumer's existing load callback
re-arms it. Hotkey observers are fenced on removal and never register schemas.
The runtime drops queued key edges when input capture starts after the edge.

## Automated evidence

- Existing portable native runner: all 27 suites passed, including modern API,
  Papyrus marshaling, runtime lifecycle, view recovery and input tests.
- Legacy schema/view compatibility: 54 checks passed, including the original
  SSSE schema, F7 default, existing F8 override, sparse persistence and unknown fields.
- Existing legacy native bridge suite passed (ABI 1.0–1.8 and settings callbacks).
- New legacy Papyrus suite: 100 checks passed against the real native bindings and
  MessageBridge with VM/Settings test doubles. Includes market replay, trade calls,
  typed charts, timeout >30s, single-use replies, per-view correlation, capacity,
  repeated open, load failure/success, unregister/re-register, dead receivers,
  F7/rebind/capture, concurrent publication and malformed input.
- Four JavaScript tests passed, with zero skips. One executes the installed
  Stock Exchange script in memory with its original `bind`, `act` and `req`
  functions; only DOM rendering/boot is substituted by a harness. This proves
  browser contract consumption, not rendering, Papyrus execution or game trades.
- Current OSF Settings provider tests: 47 checks passed against its production
  services, including static ownership, provider replacement, persistence failure,
  key changes, shared input blocks and unsubscribe.
- Full OSF UI native build and Papyrus Compiler 4.7.0.5 compilation passed.
- Package ownership and binary checks pass: the actual DLL accepts modern API 2.0
  and legacy bridge 1.0 through 1.8; the bridge vtable reports 1.8. The actual PEX
  contains 55 Global Native functions whose complete signatures match the shipped
  source and independent frozen legacy expectations. Both modern and legacy shared
  assets plus the matching browser host are included. No consumer or Settings DLL,
  consumer schema, legacy settings menu, or retired wrapper script is packaged.

The historical `dist/OSF-UI-v2.0.0.zip` was independently extracted into audit
scratch. Its DLL SHA-256 is
`2dc2d24581c57d2e929f66b94df31608e76ff5164ebf671dbf03741601ba7884`.
Direct export calls accept modern API **1.0** and reject **2.0**. The new packaging
verifier correctly rejects it. This old archive is preserved, not replaced.

The five vendored Settings SDK headers match the current clean OSF Settings
checkout at `c0afdf0ade3ce6b6bb0c45875302c16551d09a22` byte for byte. Runtime
acceptance needs a Settings build from this compatible source/layout; product
version 1.0.0 alone is insufficient to identify a development build.

## Historical adapter candidate artifacts and hashes

Built from OSF UI `dev` at `2f5f68d2f5abca374924f71ef845c940e41fb6af` plus
the uncommitted changes listed below, in `releasedbg` mode. The candidate archive
is `dist/OSF-UI-v2.0.0-ssse-compat.zip`; its adjacent `.zip.sha256` records the
archive checksum. The completed ZIP was extracted to a fresh audit directory and
passed ownership, DLL acquisition/vtable and compiled PEX checks again. All 12
archive files match validated staging byte for byte; the adjacent
`OSF-UI-v2.0.0-ssse-compat.verification.json` records their individual hashes.

| Artifact | SHA-256 |
| --- | --- |
| Candidate ZIP | `5bd4ba7c5c87c51882d6cf14a7f744bf3b1a3de2fba8e456df2b0ee7fc92e4b2` |
| `Data/SFSE/Plugins/OSFUI.dll` | `f91bd9d6d33e54e1dad15fbb0bcdc6c88f81783430144ec5313f949126106028` |
| `Data/Scripts/OSFUI.pex` | `ef4dfbdcfdbde7826f84de119df7a0af5c673f60fe88701269c1e92d3d4989f0` |
| `Data/SFSE/Plugins/OSF/UI/bin/osfui_webview2_host.exe` | `678c6f1f7303fa55c8d4030e83035aef6536c36b6ac5c05e48fc89674bb7a928` |

Logs: `build/ssse-audit/native-tests-final.log`, `compat-tests.log`,
`compat-rebuild.log`, `legacy-papyrus-final.log`, `web-tests.log`,
`settings-provider-build.log`, `package-final.log`, and `old-binary-rejection.log`.
The focused final log supersedes the earlier 84-check Papyrus run in the combined
compatibility log. The 21-file consumer hash inventory is `consumer-hashes.json`.

Rerun from the OSF UI repository in PowerShell (Git Bash is needed for the
portable native runner). Save/restore deployment environment variables around
manual xmake commands; the package script already does this itself.

```powershell
$savedModsPath = $env:XSE_SF_MODS_PATH
$savedGamePath = $env:XSE_SF_GAME_PATH
try {
    $env:XSE_SF_MODS_PATH = $null
    $env:XSE_SF_GAME_PATH = $null
    bash tests/native/run.sh
    if ($LASTEXITCODE) { throw 'Portable native tests failed' }
    foreach ($target in 'osfui-compat-tests', 'osfui-legacy-bridge-tests',
            'osfui-legacy-papyrus-tests', 'osfui-api-version-tests') {
        xmake build -y $target
        if ($LASTEXITCODE) { throw "Build failed: $target" }
        xmake run $target
        if ($LASTEXITCODE) { throw "Tests failed: $target" }
    }
} finally {
    $env:XSE_SF_MODS_PATH = $savedModsPath
    $env:XSE_SF_GAME_PATH = $savedGamePath
}
$env:OSFUI_SSSE_ROOT = "C:\Modding\Starfield\MO2\mods\X-2357's Settled Systems Stock Exchange"
node --test tests/native/compat_web_tests.cjs tests/native/ssse_consumer_web_tests.cjs
if ($LASTEXITCODE) { throw 'Legacy JavaScript tests failed' }
./tools/package.ps1 -Version 2.0.0 -Tag ssse-compat
./tests/package/test-package.ps1 -DataRoot ./build/package/staging/Data
```

For archive verification, extract the candidate into a fresh directory and pass
its `Data` directory to `test-package.ps1`. The verifier requires Windows x64 and
Python 3. Rebuilds can have different hashes because binaries/PEX/ZIP carry build
metadata; the checksums above identify this specific candidate.

## Disposable-profile in-game acceptance — UNVERIFIED

Use a separate portable MO2 instance/profile and local copied save directory.
Do not enable this candidate in the regular play profile or overwrite its mods,
INI files or saves. Keep the original consumer directory immutable; copy its
package into the disposable profile without modifying its contents.

1. Record Starfield 1.16.244.0, matching SFSE and Address Library v21, Windows
   WebView2 runtime, candidate UI ZIP/DLL/PEX/host hashes below, and the exact
   compatible OSF Settings DLL hash/source. Install the UI ZIP as its own test mod
   and ensure only one UI and one Settings package win conflicts. Verify virtual
   `Scripts/OSFUI.pex`, `SFSE/Plugins/OSFUI.dll`, both shared kits and the browser host
   resolve to this candidate. Include Stock Exchange's original ESM/scripts/assets
   and its required masters; do not install a Stock Exchange port/modern schema.
2. Use a copied save where the Stock Exchange quest is running, or initialize it
   in a new disposable game. Capture the Papyrus `[SSSE View] registered:` trace:
   actions/requests/hotkey tokens must all be nonzero and OSF UI version 20000.
   Check for missing-native/signature failures. Its `Publish:` trace must report
   a ready market and nonzero stocks.
3. Press F7 once in gameplay. The exchange should open and show real quotes,
   stable company names and holdings/credits, not just its HTML shell or demo
   data. Also open it through its existing Broker Console/launcher paths.
   Record the market state and a screenshot.
4. Buy a small quantity and sell part of it. Compare holdings, actual inventory
   credits, fees and the order result. Expect one action trace and one fill per
   click. Check a rejected order as well. Do not infer success from an action
   trace alone; the market and credit transfer must have changed correctly.
5. Open company charts and switch Day/Week/Month/Year/5y. Confirm returned values
   display for intraday, candles and thinned closes; check option quote and paged
   positions too. Exercise a busy VM long enough to produce a >30s but <60s reply.
   A valid late-within-budget reply must still display. For rejected/expired
   requests, confirm the page exits its pending state and a subsequent request
   succeeds; use a temporary test-only probe view/quest if deterministic silence
   is needed, keeping the Stock Exchange package unchanged.
6. Close/reopen ten times using Esc/Close and F7. Repeat after an in-flight chart
   request: the closed request must not settle a new page request. Ensure no
   duplicate registration/action traces and no progressively increasing callback
   count. F7 inside the captured web menu must not dispatch `OnHotkey`, enter text,
   open a second view, or show a browser caret-browsing prompt.
7. Rebind `openExchange` to F8 in OSF Settings. F7 should stop opening it; F8 should
   open it once. Capture/rebinding, other input-owning menus and held-key repeats
   must not invoke the shortcut. Confirm the original legacy
   `SFSE/Plugins/OSFUI/settings/values/x2357.ssse.json` preserves the override;
   restart the disposable game and verify persistence. Restore F7 afterward.
8. Save to a disposable slot, trade/change the market, then load the earlier
   copied save (also quickload and load from main menu). Expect one successful
   re-arm and fresh publication per load. Prior-session tokens/replies must have
   no effect; F7, buying/selling and charts must work again. Check that no stale
   quotes/holdings remain once the new publication completes.
9. If testing browser recovery, stop only the host process owned by this
   disposable game session, reopen, and confirm retained replay with no old
   request result. Exit the test game; archive logs/screenshots, input/package
   hashes, save identity and pass/fail per step, then discard the test profile.

Until these steps are recorded, the acceptance claim is **automated contract
compatibility, with game behavior still unverified**. The restored functions do
not emulate all historical Papyrus settings APIs, transient pushes or localization.

## Changed files

- `src/Compat/V1/PapyrusAdapter.{h,cpp}`: isolated legacy bindings, registry, state,
  request ledger and Settings key observers.
- `src/API/PapyrusApi.cpp`: legacy binding and save/main-menu lifecycle hooks.
- `src/Bridge/MessageBridge.{h,cpp}`: internal per-defer deadline and gate/liveness
  queries; modern default unchanged.
- `src/Bridge/RuntimeBridge.cpp`, `src/Runtime/Runtime{,Frame}.cpp`,
  `src/Views/RuntimeViews.cpp`: source-view admission, replay/pump and close/navigation cleanup.
- `src/Compat/V1/web/osfui.js`, `data/Scripts/Source/OSFUI.psc`: state/reset/timeout
  translation and frozen native declarations.
- `tests/native/legacy_papyrus_tests.cpp`, `ssse_consumer_web_tests.cjs`,
  `compat_web_tests.cjs`, `compat_v1_tests.cpp`, `stubs/RE/B/BSScriptUtil.h`,
  `fixtures/compat-v1/{ssse-settings.json,README.md}`, `run.sh`, `xmake.lua`: regression
  fixtures, exact callback-shape assertions and test build wiring.
- `tests/package/{test-package.ps1,verify_binary.py}`, `tools/package.ps1`: actual
  compiled-artifact gate and archive hash sidecar.
- `docs/compatibility-v1.md`, this record: scope, contracts and acceptance evidence.

Unrelated untracked `packaging/nexus/` files that appeared during this work were
left untouched and are not part of the compatibility change or candidate ZIP.
