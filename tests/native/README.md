# Native host-side unit tests

Compiles the **real** runtime and API sources under test — `SettingsStore`,
`Json`, `SettingsModule`, `MessageBridge`, `SettingsMirror`,
`SettingsSubscriptions`, `HotkeyService`, `KeyNames`, `HotkeySubscriptions`,
`BridgeApi`, `ControlMapPolicy`, `LocalizationService`, `ViewManifest`, `DiagnosticsModule`,
and `DiagnosticsReconciler` — on the developer's desktop
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

The shared WebView2 pipe is Windows-specific and has its own regression target:

```powershell
xmake build wv2-pipe-tests
xmake run wv2-pipe-tests
```

It stress-tests create-before-launch acceptance, kernel peer-PID queries,
hello/read deadlines, close during server accept, blocked reads and writes, and
clean reopening against the real named-pipe implementation.

## How it works

The plugin build force-includes `src/pch.h` (CommonLibSF + REX). Here,
`stubs/pch.h` substitutes it: the same std umbrella plus a minimal `REX::INFO/
WARN/ERROR/DEBUG` stub matching CommonLibSF's CTAD-struct call syntax. Logged
lines are recorded in `REX::test::Entries()` so tests can assert on warnings
(e.g. duplicate-id resolution). `OSFUI::Log` (from `core/Log.h`) is stubbed in
the test file itself — `src/core/Log.cpp` pulls game deps and is not compiled.

## Scope

The portable `run.sh` suites have no game/SFSE/browser-SDK dependencies. The
Windows pipe suite is built separately through xmake. Currently:

| Test | Covers |
|---|---|
| `wv2_pipe_tests.cpp` (Windows/xmake) | Real named-pipe lifecycle: create-before-connect, kernel peer identity, total read deadlines, close-during-accept, cancellation of blocked read/write I/O before handle release, and clean session reuse |
| `wv2_bounded_queue_tests.cpp` | Shared transport queue policy: hard capacity, order-safe tail coalescing, bootstrap prepend ordering, close wakeup, and reusable reset |
| `settings_store_tests.cpp` | `SettingsStore` (mcm-design.md §8.3): load/overlay/clamp, deterministic duplicate-id resolution, multicast listeners, incremental `RegisterSchema` + Source precedence, per-mod replay, `RemoveMod`, `GetValue`/`GetSettingType`/`GetSource`, `ValidateSchemaShape` (the ABI's synchronous gate), generation counter, sparse write-behind persistence (debounce window, prune-to-default on load, teardown flush) |
| `settings_module_tests.cpp` | `SettingsModule` + `MessageBridge` (§8.5): subscribe-on-read via real `ui.command` envelopes, `settings.changed` push to all subscribers, caller-only acks, `settings.persisted` on the write-behind flush, `settings.data` re-broadcast on registry shape change, `OnBridgeDown` teardown |
| `runtime_diagnostics_tests.cpp` | `RuntimeDiagnostics` reconciliation policy: settings issue severity/lifecycle, order-stable compatibility dedupe and resolution, and view retry/failure/recovery transitions |
| `settings_mirror_tests.cpp` | `SettingsMirror` (§8.2): any-thread typed getters over the value mirror, value-shape mismatches, `GetString` buffer semantics, `Rebuild` from the store document, integration with the real store's change/registry listeners |
| `settings_subscriptions_tests.cpp` | `SettingsSubscriptions` (§8.2): replay-on-subscribe (one-shot, mirror snapshot), queued change dispatch + per-mod routing, unsubscribe (incl. from inside a callback), re-entrant subscribe, subscribe-before-registration via the real store's per-mod replay |
| `hotkey_service_tests.cpp` | `HotkeyService` (§9), wired exactly like `Runtime::BuildModules` over the real store + `ResolveKeyName`: registry rebuild on rebind and on registry shape change, suppression while the overlay captures input or a rebind is armed, duplicate-binding fan-out, and the informational conflict data embedded in `SettingsStore::Data()` |
| `hotkey_subscriptions_tests.cpp` | `HotkeySubscriptions` (§9), the `SubscribeHotkey` ABI bookkeeping: per-(mod, key) routing, queued fire dispatch, unsubscribe (incl. from inside a callback), re-entrant subscribe |
| `bridge_api_tests.cpp` | `BridgeApi`: ABI 2.0 constants, strict `RegisterSend`/`RegisterRequest` routing, ABI 1.x refusal diagnostics, retained state, plugin endpoint-shape enforcement, first-wins duplicate refusal, unregister-then-reregister replacement, qualified `RegisterView` ids, discovery-aware `RequestMenu` validation, pre-ready delivery to a first lazy bridge, and the registry-apply/dispatch round trip through a real `MessageBridge`. **Note:** `BridgeApi` is a process singleton, so its sections share state and run in order |
| `papyrus_action_tests.cpp` | Papyrus events/state/requests plus menu-native behavior: legacy and fixed-name callback registration, typed retained state replacement/replay/teardown, one-shot correlated replies/rejection/timeout, bounded queues, open/close results, and `BSFixedString` case folding |
| `papyrus_form_tests.cpp` | Form references across the bridge (protocol 1.3, `docs/form-references-design.md`): `PushFormsToView`'s capture-FormIDs/serialize-at-drain split against a stub `TESForm` registry (identity fields, `FORM_ENUM_STRING` signatures + numeric fallback, null-slot preservation for `None`/deleted forms, empty pushes, validation and cap shared with `PushToView`) and the `GetFormById`/`GetFormsById` resolvers (decimal + hex parse matrix, quiet stale references, bulk order/length) |
| `control_map_policy_tests.cpp` | Conservative live-ControlMap classification and the semantic active-context precedence used by scoped hotkey dispatch |
| `scan_code_tests.cpp` | The physical key identity core: `ComposeScanCode`'s message-quirk normalization (Pause/NumLock/PrintScreen), the `kNamedScans` full-table name round-trip and ≤16-char constraint, W3C `KeyboardEvent.code` aliases, and the frozen legacy VK resolver the values migration reads |
| `key_label_tests.cpp` | The localized keycap-label pipeline (`input/KeyLabels`): fixed short forms for non-printing keys (localizable via `chrome.keys.*`), layout glyphs for printable keys, the fallback chain, and US/German-QWERTZ layout fixtures (Z/Y swap, umlauts, dead keys, the ISO `<>` key) |
| `localization_service_tests.cpp` | `LocalizationService`: the English-source catalog and the locale fallback rules (exact locale → base language → authored English) |
| `view_manifest_tests.cpp` | `ViewManifest`: canonical manifest accents and the `readySignal` native-bridge requirement/fallback |

Every suite is assert-style and exits with its own failure count; `run.sh` sums
the portable suites. Adding one means adding a row to `SUITES`; that list drives
compilation, linking, and execution so a suite cannot be built but silently skipped.

These suites verify runtime and API logic, not the plugin: renderer/compositor
backends, ABI wiring into SFSE, threading (the main-thread pump), and in-game
behavior still need the Windows build.
