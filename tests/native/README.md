# Native host-side unit tests

Compiles the **real** runtime and API sources under test — `SettingsStore`,
`Json`, `SettingsModule`, `MessageBridge`, `SettingsMirror`,
`SettingsSubscriptions`, `HotkeyService`, `InputRouter`, `HotkeySubscriptions`,
`BridgeApi`, `VanillaKeys`, `LocalizationService` — on the developer's desktop
toolchain (macOS/Linux clang or any C++23 compiler) and runs them without
Windows, xmake, SFSE, or the game.

This is the native half of the test story. The web half is
[`frontend/test/`](../../frontend/README.md) (vitest: protocol, settings and
keybinds logic, and the generated-output gates); the two share no code and run
as separate CI jobs.

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

Only sources with no game/SFSE/browser-SDK includes can live here. Currently:

| Test | Covers |
|---|---|
| `settings_store_tests.cpp` | `SettingsStore` (mcm-design.md §8.3): load/overlay/clamp, deterministic duplicate-id resolution, multicast listeners, incremental `RegisterSchema` + Source precedence, per-mod replay, `RemoveMod`, `GetValue`/`GetSettingType`/`GetSource`, `ValidateSchemaShape` (the ABI's synchronous gate), generation counter, sparse write-behind persistence (debounce window, prune-to-default on load, teardown flush) |
| `settings_module_tests.cpp` | `SettingsModule` + `MessageBridge` (§8.5): subscribe-on-read via real `ui.command` envelopes, `settings.changed` push to all subscribers, caller-only acks, `settings.persisted` on the write-behind flush, `settings.data` re-broadcast on registry shape change, `OnBridgeDown` teardown |
| `settings_mirror_tests.cpp` | `SettingsMirror` (§8.2): any-thread typed getters over the value mirror, value-shape mismatches, `GetString` buffer semantics, `Rebuild` from the store document, integration with the real store's change/registry listeners |
| `settings_subscriptions_tests.cpp` | `SettingsSubscriptions` (§8.2): replay-on-subscribe (one-shot, mirror snapshot), queued change dispatch + per-mod routing, unsubscribe (incl. from inside a callback), re-entrant subscribe, subscribe-before-registration via the real store's per-mod replay |
| `hotkey_service_tests.cpp` | `HotkeyService` (§9), wired exactly like `Runtime::BuildModules` over the real store + `ResolveKeyName`: registry rebuild on rebind and on registry shape change, suppression while the overlay captures input or a rebind is armed, duplicate-binding fan-out, and the informational conflict data embedded in `SettingsStore::Data()` |
| `hotkey_subscriptions_tests.cpp` | `HotkeySubscriptions` (§9), the `SubscribeHotkey` ABI bookkeeping: per-(mod, key) routing, queued fire dispatch, unsubscribe (incl. from inside a callback), re-entrant subscribe |
| `bridge_api_tests.cpp` | `BridgeApi` (api-freeze items 1 + 3): plugin command-shape enforcement (`<author>.<modname>.<name>`, ABI 1.6), first-wins duplicate refusal, unregister-then-reregister replacement, qualified `RegisterView` ids, and the registry-apply/dispatch round trip through a real `MessageBridge`. **Note:** `BridgeApi` is a process singleton, so its sections share state and run in order |
| `vanilla_keys_tests.cpp` | `VanillaKeys` (§9 "vanilla hotkeys"): the curated `vanillakeys.json` defaults table and the engine controlmap overlay parser, with fake resolvers standing in for the two platform facts (key name → VK, DIK scan → VK) |
| `localization_service_tests.cpp` | `LocalizationService`: the English-source catalog and the locale fallback rules (exact locale → base language → authored English) |

Every suite is assert-style and exits with its own failure count; `run.sh` sums
them. Adding a suite means adding a `compile` call **and** the binary name to
the runner loop at the bottom of `run.sh` — a suite left out of that list
compiles in CI and is never executed.

These suites verify runtime and API logic, not the plugin: renderer/compositor
backends, ABI wiring into SFSE, threading (the main-thread pump), and in-game
behavior still need the Windows build.
