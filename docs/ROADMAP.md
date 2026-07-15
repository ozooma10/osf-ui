# OSF UI — Roadmap (P1 / P2 / P3)

Self-contained handoff of the remaining work toward feature parity with the
ideas of Prisma UI 1.4.1 (Skyrim). Derived from the parity audit. Status
reflects the state **after** the public modder API was removed and the project
was rebranded off Prisma to OSF UI (2026-06-25).

Prisma UI reference source (for "how Prisma does it"):
`C:\Modding\Skyrim\PrismaUI_SKSE`. The full audit + reverse-engineered Prisma API
spec are in `.audit/` (gitignored — copy them manually if you want them on the
other machine: `PARITY_AUDIT.md`, `PRISMA_API_SPEC.md`).

Status key: ✅ done · 🔧 in progress · ❌ not started · 🟡 partial · 🧹 cleanup · 🔬 experimental/unproven

---

## Already done (don't redo)

- ✅ **Brand = OSF UI** (namespace `OSFUI`, data folder `OSFUI`, `window.osfui`,
  DLL `OSFUI.dll`, MO2 target `OSF UI`). Heavily inspired by Prisma UI but uses
  none of its names, branding, or API. (A short-lived 2026-06-24 experiment that
  rebranded to "PrismaUI SF" + added a Prisma-source-compatible public C++ API
  was reverted on 2026-06-25.)
- 🧹 **Public modder C++ API removed.** The Prisma-compatible `PRISMA_UI_API`
  surface (`src/api/`, `sdk/PrismaUI_API.h`, `RequestPluginAPI`) was deleted with
  the rebrand revert. The renderer keeps generic per-view primitives
  (`EvaluateScript`/`CallJsFunction`/`RegisterJsFunction`/`SetConsoleHandler`/
  `SetViewHidden`/`SetViewOrder`/`SetScrollPixelSize`/`DestroyView`), but there is
  no public entry point for other SFSE plugins. The config/declarative path is
  the only supported way to host views. If a native API is wanted again, design
  it under the OSF UI brand (no Prisma compatibility constraint).

- ✅ **Mouse-wheel scroll into views** — verified in-game 2026-06-24. Full path
  wired: `RI_MOUSE_WHEEL` in `OverlayInputHook::RouteRawMouse` →
  `Runtime::OnHostMouseWheel` (routes at the virtual cursor) → Ultralight
  `ScrollEvent` (`kType_ScrollByPixel`) on the worker in `UltralightWebRenderer`,
  using per-view `ViewState::scrollPx` (default 28).

- 🟡 **Hardware (OS) cursor** — implemented 2026-07-01, needs in-game
  re-verification. While captured, the real Windows pointer is shown
  (`input/HardwareCursor`: bounded `ShowCursor` raises + `WM_SETCURSOR`
  override + client-rect clip, self-healing on every mouse packet), and the
  page's CSS `cursor` maps to the matching system cursor (Ultralight
  `OnChangeCursor` → `CursorShape` → `IDC_*`). Replaces the page-drawn `<div>`
  pointer, which trailed the hand by the full Ultralight paint + upload +
  Present pipeline. **Routing lesson from the first live test (2026-07-01):
  the game's raw-input registration suppresses the legacy `WM_MOUSE*` stream
  (RIDEV_NOLEGACY behavior), so the first cut — routing from `WM_MOUSEMOVE`/
  `WM_LBUTTON*` — got no mouse input at all (clicks dead, page never saw
  moves). Mouse is now routed exclusively from raw `WM_INPUT`, with the
  position read live from `GetCursorPos` while the hardware cursor is active
  (`Runtime::OnHostMouseAbsolute`, client→view uniform scale; the renderer
  divides by `device_scale` per the SDK Sample 7 convention).**
  `config.hardwareCursor=false` restores the raw-delta virtual cursor (no
  visible pointer; debug only). To verify live: pointer appears/disappears
  with F10, hover/click/scroll land where the pointer is, no double cursor
  with `focusMenu` on (engine arrow — see verification doc §B), no recenter
  fights from the engine.

## P0 — in flight

- _(none — the P1 text-input cluster (clipboard → WM_CHAR) is complete and
  verified, and the gamepad-leak cluster (FocusMenu + ControlLayer + engine
  gamepad routing) is verified and on by default. Remaining P1: full
  HDR/frame-gen swapchain output.)_

---

## P1 — make hosted views genuinely usable

| Item | Status | Where / notes | Effort |
|---|---|---|---|
| **Clipboard copy/paste** (CF_UNICODETEXT) | ✅ | Done + verified in-game 2026-06-24. `WinClipboard : ul::Clipboard` (registered in `UltralightWebRenderer::SetupPlatform`) bridges to Win32 `Get/SetClipboardText`/`ClearClipboard` in `WindowsPlatform.cpp`. Copy/cut/paste flow through the page's own Ctrl shortcuts — no WndProc/JS shim needed. | M |
| **WM_CHAR / WM_UNICHAR keyboard path** | ✅ | **OS char stream adopted — verified in-game 2026-06-25.** The VK stream now drives only `RawKeyDown`/`KeyUp` (RawKeyDown still carries `unmodified_text`, so Ctrl+A/C/V/X/Z + clipboard keep working); text *entry* comes from `WM_CHAR`/`WM_UNICHAR` → `OverlayInputHook` (surrogate halves combined, control chars filtered) → `Runtime::OnHostChar` → `InjectCharEvent` → a `kType_Char` event carrying the layout-/dead-key-/AltGr-resolved codepoint. Both streams share the one `toInput` FIFO, so each key's RawKeyDown precedes its Char (what WebCore needs). Held printable keys now auto-repeat (OS WM_CHAR repeat), matching native text fields. Remaining (separate scope): true IME with a composition window is still greenfield. | L |
| **Per-view Show/Hide for config/declarative views** | ✅ | Done + verified in-game 2026-06-24. `setViewHidden` bridge command → `Runtime::SetViewHidden(id, hidden)` → renderer per-view `hidden` flag, independent of the global `_visible` toggle. Also fixed a latent compositor bug: hide/show now forces one recomposite (`compositeDirty`) so a *static* hidden view doesn't linger. The same recomposite fix was later applied to API `SetViewOrder` (2026-06-25), so a runtime reorder of static views repaints too. | M |
| **Load-lifecycle events to callers** (OnFinishLoading / OnFailLoading) | ✅ | Internal hook done + verified 2026-06-24: finish/fail routed worker→pump→`Runtime::OnViewLoad`, tracked per-view (`ViewLoadState` + `GetViewLoadState`) as the trigger for **P2 URL crash-recovery** (TODO marked there). No public API surface — this is an internal hook only. | M |
| **Prove FocusMenu + ship ControlLayer on-by-default** | ✅ | Done + verified in-game 2026-07-02 (re-exercised 2026-07-15). `OSFUI_FocusMenu` (Route-A stack admission, hardened creator, clean kHide teardown), `ControlLayer` (BSInputEnableManager — freezes KB + mouse-look + **gamepad**), `SimPause`, `FreeCursor`, and `EngineInput` gamepad routing are all on by default in `Config.h` and the shipped config. **The gamepad leak is FIXED.** See `docs/verification-focusmenu-controllayer.md`. | L |
| **HDR / 10-bit / multi-format PSO + frame-gen swapchain** | 🟡 | **Detect-and-degrade shipped 2026-07-01 (unverified pending in-game test):** `D3D12Compositor` now builds one PSO per *supported* backbuffer format (8-bit UNORM RGBA/BGRA — the verified SDR path) instead of first-seen-wins, and refuses unsupported formats (R10G10B10A2, FP16, `_SRGB`) **per swapchain** with a once-per-change WARN naming the format + the output's HDR state — it no longer renders wrong colors into an HDR buffer it happened to see first. Symptom on HDR rigs: overlay invisible + the log line; workaround: SDR. **Full-support scope (not started):** (a) HDR10/PQ — sRGB→linear→Rec.2020→PQ encode in the pixel shader + a paper-white-nits setting; (b) scRGB FP16 — sRGB→linear × paperWhite/80; (c) frame-gen — pick the scanned-out swapchain (needs in-game evidence of which chain reaches the display under DLSS-G); (d) 10-bit **SDR** (R10G10B10A2 + G22) could be whitelisted after one in-game check. | L |

---

## P2 — resilience & tooling

| Item | Status | Where / notes | Effort |
|---|---|---|---|
| **Per-view Destroy/teardown for config views** | 🟡 | One real driver landed 2026-07-01: crash-recovery exhaustion destroys the view (`Runtime::OnViewLoad` → `DestroyView`) and unregisters its surface (`MenuController::Unregister`) so a dead view can't be reopened. Other lifecycle drivers (e.g. destroy-on-close to reclaim WebKit memory) are a design decision — do menus stay warm across F10 toggles or not — and deliberately not taken yet. | M |
| **URL crash-recovery** | 🟡 | **Landed 2026-07-01 (unverified pending in-game test).** A failed main-frame load (`Runtime::OnViewLoad`) schedules bounded reloads with backoff (3 attempts: 2s/5s/15s, driven from `Tick` via `DriveRecovery`; reload = `LoadView` again, which recreates the `ul::View`, + `Resize` back to the output size). Success clears the strike count; exhaustion destroys + unregisters the view (see row above). Scope note: this covers **load failures** — a mid-life JS/renderer fault emits no signal today, so Prisma-style fault recovery would first need a fault *detector* (e.g. worker heartbeat), not just this reload path. | M |
| **Native present-time DrawCursor sprite** | ❌ | The HARDWARE cursor (shipped 2026-07-01, above) covers the mouse case with zero lag. A compositor-drawn sprite at the latest cursor pos remains useful as the pointer for a future GAMEPAD-driven cursor and for passive/crashed views where no OS pointer is engaged. Ref Prisma `ViewRenderer::DrawCursor` (cursor.png). | M |
| **DevTools / Inspector** | ❌ | No inspector/DevTools today. Real impl needs a 2nd compositor layer + input routing (harder in D3D12 than Prisma's D3D11 SpriteBatch), and the remote inspector is gated behind an Ultralight **Pro** license. | L |
| **GPU-side N-texture z-order compositing** | ❌ | Multi-view is currently **CPU-blended** into one texture (`UltralightWebRenderer.cpp` CompositeViews); the SRV heap is sized for one texture. Move to GPU compositing if many/large overlapping views become common. | L |

---

## P3 — ergonomics & cleanup

| Item | Status | Where / notes | Effort |
|---|---|---|---|
| **NanoID-style handles** | ◆ | Moot until/unless a native API returns — view identity is the manifest id today. Revisit if programmatic views come back. | S |
| **Per-view focus contract for config views** | 🟡 | Config views focus via the `Tab`-cycle (`Runtime::CycleActiveView`). If a native API returns, give it an explicit Focus/HasFocus contract instead of overloading the cycle. | M |
| **Delete dead input scaffolding** | ✅ | Done 2026-07-01, with a scope correction: `InputRouter` was **not** dead (it's the live toggle/Esc/key-routing decision point fed by the WndProc — only its "NOTHING CALLS THIS YET" comment was stale; comment fixed, unused mouse/text methods + `MouseButton` enum removed). `UiInputHook`'s observe-only vfunc hook WAS dead and is deleted; the mandatory layout guard survives as `input/UiLayoutGuard.{h,cpp}` and still gates all UI integration. | S |
| **Fix stale docs/comments** | ✅ | Done 2026-07-01: README "SFSE InputMap" claim (code resolves names to VK via its own table), README/architecture.md "d3d12 is a stub" claims (verified production path), `Config.h` toggleKey/"no key hook" + inputSource comments, `Runtime.{h,cpp}` router-fed-by-UiInputHook comments, `Log.h` stub example. | S |
| **Remote http(s) URL views** | ⛔ | Deliberate **non-goal** under the JS sandbox (`docs/security-model.md`); `file:///` only. Defer indefinitely unless the threat model changes. | — |

---

## Cross-cutting risks (could move estimates)

- **Custom IMenu / FocusMenu on 1.16.244** — RESOLVED: the hand-built engine
  object (`uiMovie=null`) is admitted via Route A, survives long sessions, and
  tears down cleanly (kHide delegated to the engine base). Verified in-game
  2026-07-02.
- **D3D12 compositor** is production-shaped (verified 2026-06-12/13) but HDR
  backbuffers + frame-gen swapchain selection are untested (highest render
  uncertainty).
- **IME / Unicode** is greenfield (WM_CHAR currently blocked); any estimate is
  soft until the OS char path lands (P1).
- **Overlay coexistence** (ReShade / RTSS / Steam overlay / HDR) hook-chain
  ordering is untested. Defensive diagnostics landed 2026-07-01 (unverified
  pending in-game test): the Present-hook install logs which module owned
  slot 8 first (chaining after it), read-back-verifies the vtable write, and
  a tick-thread watchdog warns once when presents stop reaching our thunk
  (a non-chaining re-hook) — so broken stacks self-identify in the log
  instead of silently losing the overlay. Actual multi-overlay validation
  still needs the real tools installed.

## Non-code follow-ups (owner: you)
- ✅ **Branding/licensing:** resolved 2026-06-25 — the project no longer uses the
  Prisma name/brand/API. It is "OSF UI", credited as *inspired by* Prisma UI.
  README/CREDITS affiliation language updated accordingly.
- **Repo directory** is `OSF UI` and the build target is `OSF UI` (DLL
  `OSFUI.dll`); rename the GitHub repo to match if desired.
