# OSF UI 1.x compatibility removal checklist

The 1.x bridge is a temporary OSF UI 2.0.x migration aid and is removed in
**2.1.0**. The authored 2.0 shared bridge helper, public 2.0 SDK, and strict 2.0 dispatch are
not compatibility code and must remain unchanged.

Use this as the deletion checklist. The two compatibility directories contain
the behavior; the entries below are the unavoidable integration hooks that let
that behavior reach the OSF UI runtime and authoring tools.

## Delete the isolated implementations

- Delete `frontend/src/compat/v1/` and its focused frontend tests.
- Delete `src/compat/v1/` and `tests/native/v1_*`.

## Remove shared bridge helper composition and navigation selection

- `frontend/scripts/compose-helper.mjs`: remove composition and return to
  copying `frontend/src/shared-kit/osfui.js` verbatim.
- `frontend/scripts/build.mjs`, `verify-output.mjs`, and
  `frontend/test/build.output.test.ts`: remove the v1 composition hooks and
  reinstate the byte-identical core-helper assertion.
- `packages/cli/scripts/sync-public-assets.mjs` and
  `packages/cli/src/shared-assets.mjs`: stop composing or selecting the
  packaged v1 façade; regenerate `packages/cli/assets/osfui.js` from the strict
  shared bridge helper.
- `src/render/WebView2HostWebRenderer.cpp`: remove `legacyApi` from
  `NavigateMsg` and `ViewRec`.
- `tools/webview2_host/HostApp.cpp` and `GameMessages.inl`: remove the v1
  navigation include and `WithLegacyApiQuery` call.
- `packages/cli/src/browser/legacy-navigation.js`, `browser/shell.js`, and
  `harness-plugin.mjs`: remove `osfui-api=1` selection and the browser module.

## Remove the native ABI adapter

- `src/api/Exports.cpp`: remove ABI-major 1 dispatch; leave ABI 2.x dispatch
  and unrelated-major refusal intact.
- `src/api/BridgeApi.{h,cpp}`: remove the legacy command/request registries,
  pending unregister queues, `RegisterLegacy*` methods, `LegacyCaller` ledger,
  legacy request marker, and v1 typed-reply wrapper. Keep the strict
  `RegisterSend`/`RegisterRequest` paths unchanged.
- `src/runtime/MessageBridge.{h,cpp}`: remove `RegisterLegacyCommand`, the
  legacy command map, request-ID injection/auto-ack branches (including the
  legacy-view-only request-to-strict-send branch), `Gate::legacyApi`,
  `IsLegacyApiView`, and the extra `OnViewCreated` argument.
- `src/runtime/Runtime.cpp`: remove the three legacy flags passed to
  `OnViewCreated`.

## Remove the Papyrus adapter

- `src/api/PapyrusApi.cpp`: remove `Compat::V1::Papyrus::BindNatives` and the
  three narrow `RegisterLegacyAction*` / `SerializeFormForLegacyPush` hooks.
- `src/runtime/Runtime.cpp`: remove the `DrainPushes` block that emits
  `data.push`.
- `data/Scripts/Source/OSFUI.psc`: delete the six declarations marked
  `DEPRECATED 1.x compatibility`, then rebuild the shipped PEX.
- Remove the legacy assertions and adapter source from
  `tests/native/papyrus_action_tests.cpp`, `papyrus_form_tests.cpp`, and
  `tests/native/run.sh`.

## Reinstate the 2.1 compatibility boundary

- OSF UI runtime view discovery/opening must refuse an explicitly pre-2.0
  `targetVersion` before navigation and report it as unsupported.
- `packages/cli/src/config.mjs` must reject pre-2.0 projects again; remove
  `isPre2Target`, its warning, and the legacy toolchain/navigation tests. New
  scaffolds and published typings already remain 2.0-only.
- `src/runtime/RuntimeHealthCoordinator.{h,cpp}` and
  `HealthReconciler.{h,cpp}`: remove the legacy ABI/Papyrus warning
  producers and compatibility-log ledger. Keep genuinely unsupported ABI-major
  reporting.
- `frontend/src/lib/settings/health.ts`: remove the three temporary 1.x
  compatibility health issues and restore unsupported pre-2/ABI copy
  appropriate for 2.1.
- Update `CHANGELOG.md`, migration/authoring docs, `frontend/COMPATIBILITY.md`,
  and their tests so none promises 2.0.x compatibility after it is gone.

## Final verification

Search for `Compat::V1`, `osfui-api`, `legacyApi`, `RegisterLegacy`,
`compat.legacy-api`, `compat.legacy-papyrus`, and the six deprecated Papyrus
native names. Every remaining hit must describe history or an intentional 2.1
refusal test. Run the full frontend/CLI/native/build/headless matrix and then a
restarted-game pass proving 2.0 consumers still work and 1.x consumers now fail
with the documented 2.1 refusal.
