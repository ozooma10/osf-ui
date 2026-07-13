# Native host-side unit tests

Compiles the **real** runtime sources under test (`src/runtime/SettingsStore.cpp`,
`src/runtime/Json.cpp`) on the developer's desktop toolchain — macOS/Linux clang
or any C++23 compiler — and runs them without Windows, xmake, SFSE, or the game.
The native mirror of the web-side `devtools/harness/`.

```sh
./run.sh          # fetches nlohmann/json (pinned) on first run, builds, runs
```

Exit code is the failure count; `0` = all checks passed.

## How it works

The plugin build force-includes `src/pch.h` (CommonLibSF + REX). Here,
`stubs/pch.h` substitutes it: the same std umbrella plus a minimal `REX::INFO/
WARN/ERROR/DEBUG` stub matching CommonLibSF's CTAD-struct call syntax. Logged
lines are recorded in `REX::test::Entries()` so tests can assert on warnings
(e.g. duplicate-id resolution). `OSFUI::Log` (from `core/Log.h`) is stubbed in
the test file itself — `src/core/Log.cpp` pulls game deps and is not compiled.

## Scope

Only sources with no game/SFSE/Ultralight includes can live here. Currently:

| Test | Covers |
|---|---|
| `settings_store_tests.cpp` | `SettingsStore` (mcm-design.md §8.3): load/overlay/clamp, deterministic duplicate-id resolution, multicast listeners, incremental `RegisterSchema` + Source precedence, per-mod replay, `RemoveMod`, `GetValue`/`GetSettingType`, generation counter, persistence |
| `settings_module_tests.cpp` | `SettingsModule` + `MessageBridge` (§8.5): subscribe-on-read via real `ui.command` envelopes, `settings.changed` push to all subscribers, caller-only acks, `settings.data` re-broadcast on registry shape change, `OnBridgeDown` teardown |
| `settings_mirror_tests.cpp` | `SettingsMirror` (§8.2): any-thread typed getters over the value mirror, value-shape mismatches, `GetString` buffer semantics, `Rebuild` from the store document, integration with the real store's change/registry listeners |
| `settings_subscriptions_tests.cpp` | `SettingsSubscriptions` (§8.2): replay-on-subscribe (one-shot, mirror snapshot), queued change dispatch + per-mod routing, unsubscribe (incl. from inside a callback), re-entrant subscribe, subscribe-before-registration via the real store's per-mod replay |

This suite verifies store logic, not the plugin: ABI wiring, threading (the
main-thread pump), and in-game behavior still need the Windows build.
