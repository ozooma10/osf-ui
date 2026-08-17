# Simplification notes

The 2026-07 cleanup audit has been implemented; a 2026-08 follow-up audit
confirmed 37 of its 52 items done and folded the remainder into the list below.
The repository keeps only deliberate follow-ups that need a product decision, a
sanctioned in-game pass, or a larger refactor window:

- Decide whether the unused public shared-kit tokens and classes may be removed.
- Revisit the two intentionally different key-conflict models only with a shared
  product definition for their behavior.
- Consider unifying the browser mock layers only if their ownership model changes;
  the current runtime mock and built-in-view mock serve different harnesses. Note
  that `frontend/devmock/mockbridge.ts` is now the largest remaining protocol
  drift surface — a third hand-maintained implementation of the bridge semantics
  beside the native encoder and the page helper.
- Treat marketing SVG/PNG generation as a separate asset-pipeline project.
- God-object decomposition: `src/Runtime/Runtime.cpp` (input coordination, view
  presentation, and bridge publishing are candidates for extraction alongside
  `RuntimeHealthCoordinator`) and the
  WebView2 host's `struct App` (input,
  view presentation, WebView2 setup, and the ~350-line `InstallEvents`).
  Deferred until they carry test coverage that can gate a split.
- Replace `friend class RuntimeHealthCoordinator` (`src/Runtime/Runtime.h`) with
  an explicit inputs struct.
- Typed structs for the `MessageBridge::Encode*` envelope builders, plus a
  dispatch-exhaustiveness test that every `msg::` wire struct is routed and
  every route is backed by a struct.
- Decompose `SettingsStore::AddSchema` (~325 lines, still growing).
- `padnav.js` `focusFirst()` has no callers, but it sits on the frozen
  compatibility boundary; remove it only after a sanctioned in-game controller
  pass.
- Per-view delivery of the v1 helper façade (so 2.0 views stop downloading the
  composed compat helper) is blocked on WebView2 not raising
  `WebResourceRequested` for folder-mapped content — see
  `docs/security-model.md`. A dev-mode probe log is the gate for revisiting.

The pause-menu list debounce discussed in earlier revisions of this note has
since been removed (`src/Input/PauseMenuEntry.cpp` documents why); the adjacent
`LivePauseMenu` liveness checks are independent and remain load-bearing.
