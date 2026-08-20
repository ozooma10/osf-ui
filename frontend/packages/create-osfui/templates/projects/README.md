# Project templates

Each child directory is the project copied for one supported `surface`/`integration` preset.
Edit these files as ordinary source files; the scaffolder only replaces the explicit `__OSFUI_*__` tokens in file contents and paths.

The supported presets are `menu-papyrus`, `menu-native`, and
`settings-papyrus`. Do not expose a new surface/integration choice until its
authored directory and end-to-end scaffold test both exist.

Every browser starter uses one public bridge vocabulary: `osfui.send()` and
`osfui.request()` for browser-to-host endpoints, `osfui.on()` for transient
events, and `osfui.state.on()` for retained state. An owning view uses local
endpoint, event, and state names; qualification is reserved for cross-mod or
platform addresses. Registered Papyrus endpoints receive `{ args: [...] }`,
and `OSFUI_View.Reply` becomes the raw value resolved by `request()`.
`osfui.papyrus.call()` appears only in the recordless starter that has no load
lifecycle in which to register an `OSFUI_View` endpoint.

The three Papyrus compiler APIs (`OSFUI.psc`, `OSFUI_Settings.psc`, and
`OSFUI_View.psc`) and the native API files remain in the sibling `papyrus/` and
`native/` directories. They are synchronized SDK artifacts and are copied into
the rendered project after its authored template tree.
