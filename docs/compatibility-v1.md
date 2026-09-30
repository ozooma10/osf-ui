# OSF UI 1.6 compatibility

Compatibility ships inside OSF UI and activates automatically. Install the [matching OSF Settings 1.0.0 build](../sdk/vendor/README.md) with `OSFSettings_RequestProvidersAPI` (providers ABI 1.0). OSF Settings supplies the menu, validation, current values, launcher, and input dispatch. All old paths, formats, ABI slots, and browser translations live in OSF UI.

Mods within this baseline use their original compiled DLLs and scripts. They do
not need a new SDK, recompilation, or moved view/value files. Authors choosing to
adopt the new API should instead follow [MIGRATION.md](../MIGRATION.md). Compatibility
covers the behaviors below; it is not a promise to emulate every historical API.

The current baseline is the six packages supplied for this migration:

| Mod | Compatibility path |
| --- | --- |
| DDC | `OSFUI_RequestBridge` fallback; six schema settings and native reads/subscriptions |
| Somatic Camera SF | 18 schema settings, JSON change callbacks, named keyboard/mouse bindings |
| Field.OS AEGIS | 24 schema settings; edits written to the original file polled by its HUD |
| DevilzDad's Shop + Explorer | Legacy manifest, launcher entry, shared web kit, native request/reply handlers and gamepad events |
| AISS Companion Log | Runtime schema, F8 key subscription, private menu, native commands/events |
| Starcade OS | Runtime schema, F9 key subscription, launcher entry, native commands/events, compiled Papyrus `OpenMenu` calls |

## Settings ownership and persistence

OSF UI discovers `Data/SFSE/Plugins/OSFUI/settings/*.json` and accepts runtime `RegisterSettingsSchema` calls. It translates array groups, enum labels, and key names into the normal Settings schema and registers a provider. OSF Settings remains the single live value store. A modern static schema with the same mod ID takes precedence.

Provider edits are saved synchronously before Settings publishes them. OSF UI writes sparse, flat values to `Data/SFSE/Plugins/OSFUI/settings/values/<mod>.json`, including the old version stamps. Unknown saved fields survive. It uses a temporary file and atomic replacement; save failure leaves the live value unchanged. Existing corrupt value files are preserved and their provider is refused with a log message. Discovery does not rewrite value files.

There is no bulk migration into the new Settings values directory. The original USVFS/MO2 paths retain their profile behavior and remain readable by AEGIS. Moving a mod to a modern static schema deliberately changes which persistence path owns it; authors should provide an explicit migration when updating their mod.

Subscriptions replay typed values as per-key JSON on the OSF UI runtime tick. Key values are translated back into old names. The five physical mouse buttons use `MOUSE1` through `MOUSE5`; keyboard keys use the frozen 1.6 vocabulary. A new key with no legacy name is rejected on save rather than silently changing the binding. Key observers respect shared hotkey blocks and gameplay/menu gates; they do not consume the initiating event.

## Views and native calls

The separate `OSFUI_RequestBridge` export accepts ABI 1.0 through 1.8. It preserves the frozen ABI 1.7 vtable from the v1.6.0 SDK and appends the ABI 1.8 `SetViewState` slot, forwarding retained state to the current runtime. Somatic Camera SF's shipped DLL requests 1.8 even though its settings subscription uses the older slots; rejecting that request leaves its settings visible and editable but disconnects the mod from changes. The export never returns the new `IUI` object. New consumers use `OSFUI_RequestAPI`.

The legacy export signature, method order, callback signatures, payload layouts,
and enum values stay frozen even when the adapter implementation changes. The
new API evolves independently under the [2.0 API contract](api-contract.md).

Legacy views are discovered only under `Data/SFSE/Plugins/OSFUI/views/<mod>/<view>/`. Modern view IDs win collisions. Old `hub: true` menus join the OSF Settings launcher; private views remain private. Under MO2, the legacy tree gets its own immutable, leased cache visible to the browser host. The DLL and browser host share a private protocol version and must be deployed together. Legacy assets require restart to refresh, including in developer mode.

The old shared stylesheet and adapted JavaScript helper are shipped at the original `shared/` URLs. The helper preserves `ready`, `available()`, `send`/`emit`, `request`/`call`, event payloads, typed request replies, and gamepad messages over the new transport. `RegisterCommand` supports the baseline's fire-and-forget sends; the old command auto-ack/request-ID injection contract is outside this baseline. The old settings-browser endpoints, Papyrus settings APIs, automatic settings-to-web forwarding, localization catalogs, and later modular 1.x exports are not emulated.

`OSFUI.OpenMenu` and `CloseMenu` remain available to already compiled scripts. Their old default `osfui/settings` target opens/closes OSF Settings. The old HTML Mods/Settings/Keybindings pages are not shipped.

## Verification

The focused native tests use the supplied schemas/manifests and cover translation, sparse values, unknown-field preservation, failed saves, ABI request layout, and modern-view precedence. The bridge tests exercise the real legacy export, ABI 1.8 acquisition, initial settings replay, F4-to-F6 change callbacks, unsubscribe, and retained-state forwarding. Provider tests cover ownership, replacement, rollback, mouse bindings, shared blocks, and unsubscribe. The JavaScript test checks transport translation without launching a browser. Run `xmake test "osfui-compat-tests/*"`, `xmake test "osfui-legacy-bridge-tests/*"`, and `node --test tests/native/compat_web_tests.cjs` from the repository root.

Fresh in-game acceptance remains required for each package: edit/reload/persist settings, check AEGIS's polling, exercise F8/F9 and mouse capture, open the two launcher views, request shop results, and launch Starcade from its existing scripts. Static tests and successful deployment do not establish those outcomes.

Use the original, unmodified consumer packages for that acceptance and record
their versions/checksums alongside the OSF UI and OSF Settings candidate builds.
Rebuilding a legacy consumer against a different SDK does not establish binary
compatibility with its shipped version.
