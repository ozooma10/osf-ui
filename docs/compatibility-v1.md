# OSF UI 1.x compatibility

Compatibility ships inside OSF UI and activates automatically. Install the [matching OSF Settings 1.0.0 build](../sdk/vendor/README.md) with `OSFSettings_RequestProvidersAPI` (providers ABI 1.0). OSF Settings supplies the menu, validation, current values, launcher, and input dispatch. All old paths, formats, ABI slots, and browser translations live in OSF UI.

Mods within this baseline use their original compiled DLLs and scripts. They do
not need a new SDK, recompilation, or moved view/value files. Authors choosing to
adopt the new API should instead follow [MIGRATION.md](../MIGRATION.md). Compatibility
covers the behaviors below; it is not a promise to emulate every historical API.

The baseline includes the six packages supplied for the migration and the original installed Stock Exchange package:

| Mod | Compatibility path |
| --- | --- |
| DDC | `OSFUI_RequestBridge` fallback; six schema settings and native reads/subscriptions |
| Somatic Camera SF | 18 schema settings, JSON change callbacks, named keyboard/mouse bindings |
| Field.OS AEGIS | 24 schema settings; edits written to the original file polled by its HUD |
| DevilzDad's Shop + Explorer | Legacy manifest, launcher entry, shared web kit, native request/reply handlers and gamepad events |
| AISS Companion Log | Runtime schema, F8 key subscription, private menu, native commands/events |
| Starcade OS | Runtime schema, F9 key subscription, launcher entry, native commands/events, compiled Papyrus `OpenMenu` calls |
| X-2357's Settled Systems Stock Exchange | Typed retained Papyrus state, actions, correlated chart/option requests, and the existing F7 key setting |

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

The legacy `SetReadyCallback` retains its single global callback: a new setter
replaces the previous registration, and a null callback clears it. It runs on
the game thread during a subsequent runtime pump while the bridge is available,
then again after availability recovery or document creation/recreation. Changes
before dispatch coalesce; it is not a per-view load or first-paint signal.
Registration never invokes the callback inline. The setter is thread-safe and
waits for an executing callback on another thread before replacing or clearing
it. Keep callback code and context alive until that call returns; when clearing
or replacing from inside the callback, the current invocation must also finish.
Callbacks may call the API, but must not wait for a worker that is clearing or
replacing them. This compatibility callback is absent from the modern SDK.

Legacy views are discovered only under `Data/SFSE/Plugins/OSFUI/views/<mod>/<view>/`. Modern view IDs win collisions. Old `hub: true` menus join the OSF Settings launcher; private views remain private. Under MO2, the legacy tree gets its own immutable, leased cache visible to the browser host. The DLL and browser host share a private protocol version and must be deployed together. Legacy assets require restart to refresh, including in developer mode.

The old shared stylesheet and adapted JavaScript helper are shipped at the original `shared/` URLs. The helper preserves `ready`, `available()`, `send`/`emit`, `request`/`call`, event payloads, typed request replies, and gamepad messages over the new transport. `RegisterCommand` supports the baseline's fire-and-forget sends; the old command auto-ack/request-ID injection contract is outside this baseline. The old settings-browser endpoints, Papyrus settings APIs, automatic settings-to-web forwarding, localization catalogs, and later modular 1.x exports are not emulated.

Legacy views that bundle their own 1.x helper, such as Console Command Center,
receive transport translation in the injected browser bridge. It initiates the
ready handshake, translates `ui.command` sends/requests, and returns legacy
envelopes with the original request ID and typed reply. The shared helper remains
supported; modern views retain the 2.0 wire format. This does not add command
auto-acks or the unsupported endpoints listed above.

`OSFUI.OpenMenu` and `CloseMenu` remain available to already compiled scripts. Their old default `osfui/settings` target opens/closes OSF Settings. The old HTML Mods/Settings/Keybindings pages are not shipped.

## Legacy Papyrus view adapter

`Compat/V1/PapyrusAdapter` binds the original `SetView*`, `ListenForViewActions`,
`ListenForViewRequests`, typed `ReplyView*`, `RejectViewRequest`, `RegisterForHotkey`
and `Unregister` signatures on `OSFUI`. The instance/static listener variants and
older scalar/list action registrations are included. Setters return void;
listeners return opaque nonzero int tokens (0 on failure); replies/rejection and
unregister return bool. The modern endpoint registry and decimal reply tokens
are independent. These declarations ship compiled in `Scripts/OSFUI.pex`.

Legacy views receive cached state as `data.state { mod, key, value }` through the
adapted helper, including greeting/recreation replay and updates while hidden.
Only owning legacy views receive Papyrus state. Browser commands derive ownership
from the source view, never a caller-supplied mod. Actions invoke
`OnOSFUIViewAction(string actionName, string[] args)`; requests invoke
`OnOSFUIViewRequest(string request, string[] args, string replyToken)`.
Argument coercion follows v1.6.0: strings pass through, integers become decimal,
floats use six fractional digits, bools become `true`/`false`, and complex/null
elements become empty strings. The historical bare-action scalar fallback is
preserved, including its one empty-string element.

Successful replies retain the `papyrus.result { value }` envelope. Request tokens
are single-use and tied to the listener, view/document and session. The default
deadline is ten seconds. The helper privately forwards an explicit positive page
timeout, bounded at 60 seconds; this allows Stock Exchange's existing 60-second
chart calls to complete beyond the modern bridge's normal 30-second deadline.
Expiry rejects with `papyrus-timeout`; missing listeners use `papyrus-unavailable`.
Unregister, close, recreation and save changes cancel the corresponding pending
requests. No late result can resolve a new document's reused request ID.

Registrations survive ordinary menu closure. Closing/reopening does not subscribe
again. Successful save loads and main-menu return clear registrations and retained
script state; scripts re-register and republish, as Stock Exchange's original
`Arm()` already does. Loading suspends dispatch; a failed load resumes the existing
session. Weak VM identities, monotonic registration tokens, idempotent action/key
registration, first-wins request ownership and dead-receiver cleanup prevent
duplicates and stale callbacks. Reset invalidates only Papyrus-owned cached keys;
it does not erase native state. If a reset follows fresh greeting replay, affected
current values are republished after the reset (or on resume if loading is
suspended). Pre-hello documents receive their current values through greeting
replay. See the [reset/replay regression record](RESET_REPLAY_VALIDATION.md).

Hotkeys observe the canonical existing OSF Settings key setting using its provider
service. No additional schema, provider, ControlMap action or persistence path is
created. The adapter folds Papyrus names for matching, then subscribes with the
original registry spelling (for example `openExchange`). Empty key filters observe
all key settings for that mod. Settings owns live binding changes, gameplay gates,
shared capture blocks and non-consuming key edges; the runtime discards queued
edges if UI capture or a transition begins before dispatch.

Bounds are 65,535 registrations, 1,024 retained keys, 256 requests overall / 32 per
view, 128 browser arguments, 4,096 native array elements, and finite numeric values.
Invalid receivers/IDs, malformed names, embedded NULs, excess arguments and invalid
timeout metadata are refused. The unsupported settings/Papyrus APIs listed above
remain outside this adapter. See the [Stock Exchange validation record](ssse-compatibility-validation.md)
for recovered contracts, package identity and the outstanding in-game gate.

## Verification

The focused native tests use the supplied schemas/manifests and cover translation, sparse values, unknown-field preservation, failed saves, ABI request layout, and modern-view precedence. The bridge tests exercise the real legacy export, ABI 1.8 acquisition, initial settings replay, F4-to-F6 change callbacks, unsubscribe, and retained-state forwarding. Provider tests cover ownership, replacement, rollback, mouse bindings, shared blocks, and unsubscribe. The JavaScript test checks transport translation without launching a browser. Run `xmake test "osfui-compat-tests/*"`, `xmake test "osfui-legacy-bridge-tests/*"`, and `node --test tests/native/compat_web_tests.cjs` from the repository root.

Fresh in-game acceptance remains required for each package: edit/reload/persist settings, check AEGIS's polling, exercise F8/F9 and mouse capture, open the two launcher views, request shop results, and launch Starcade from its existing scripts. Static tests and successful deployment do not establish those outcomes.

Use the original, unmodified consumer packages for that acceptance and record
their versions/checksums alongside the OSF UI and OSF Settings candidate builds.
Rebuilding a legacy consumer against a different SDK does not establish binary
compatibility with its shipped version.

The Stock Exchange native suite runs the real bindings and message bridge against
VM/Settings test doubles: `xmake build osfui-legacy-papyrus-tests`, then
`xmake test "osfui-legacy-papyrus-tests/*"`. Run the existing browser tests with
`node --test tests/native/compat_web_tests.cjs`. For an additional read-only run of
the original consumer's JavaScript, set `OSFUI_SSSE_ROOT` to its installed mod folder
and run `node --test tests/native/ssse_consumer_web_tests.cjs`.

Packaging also requires Python 3. `tests/package/test-package.ps1 -DataRoot <staged Data>`
checks ownership, loads the actual DLL in a disposable subprocess to negotiate
modern API 2.0 and bridge 1.0–1.8, and parses the compiled native-only Starfield PEX
to compare all signatures with the shipped source and independent frozen contracts.
`tools/package.ps1` runs this before producing the ZIP and SHA-256 sidecar.
