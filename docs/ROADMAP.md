# PrismaUI SF — Roadmap (P1 / P2 / P3)

Self-contained handoff of the remaining work toward feature parity with PrismaUI
1.4.1 (Skyrim). Derived from the parity audit. Status reflects the state **after**
the rebrand + public consumer API landed (2026-06-24).

PrismaUI reference source (for "how Prisma does it"):
`C:\Modding\Skyrim\PrismaUI_SKSE`. The full audit + reverse-engineered Prisma API
spec are in `.audit/` (gitignored — copy them manually if you want them on the
other machine: `PARITY_AUDIT.md`, `PRISMA_API_SPEC.md`).

Status key: ✅ done · 🔧 in progress · ❌ not started · 🟡 partial · 🧹 cleanup · 🔬 experimental/unproven

---

## Already done (don't redo)

- ✅ **Rebrand → PrismaUI SF** (namespace `PrismaSF`, data folder `PrismaUI`,
  `window.prisma`, DLL `PrismaUI SF.dll`). Builds + deploys to `MO2\mods\PrismaUI SF`.
- ✅ **Public consumer C++ API** (`src/api/`, PrismaUI-compatible `PRISMA_UI_API`).
  This delivered several formerly-P1/P2/P3 items **for API-created views**:
  per-view Show/Hide (`SetViewHidden`), per-view Destroy (`DestroyView`),
  runtime SetOrder, console capture (`RegisterConsoleCallback`), JS↔native
  (`Invoke`/`InteropCall`/`RegisterJSListener`), DOM-ready callback.
  ⚠ The Ultralight backend of this API is **compile-unverified** (needs the SDK —
  `xmake f --with_ultralight=true`). Verify before relying on it. See
  [docs/consumer-api.md](consumer-api.md).

- ✅ **Mouse-wheel scroll into views** — verified in-game 2026-06-24. Full path
  wired: `RI_MOUSE_WHEEL` in `OverlayInputHook::RouteRawMouse` →
  `Runtime::OnHostMouseWheel` (routes at the virtual cursor) → Ultralight
  `ScrollEvent` (`kType_ScrollByPixel`) on the worker in `UltralightWebRenderer`,
  using per-view `ViewState::scrollPx` (default 28).

## P0 — in flight

- _(none — next up is the P1 text-input cluster below: clipboard → WM_CHAR.)_

---

## P1 — make hosted views genuinely usable

| Item | Status | Where / notes | Effort |
|---|---|---|---|
| **Clipboard copy/paste** (CF_UNICODETEXT) | ✅ | Done + verified in-game 2026-06-24. `WinClipboard : ul::Clipboard` (registered in `UltralightWebRenderer::SetupPlatform`) bridges to Win32 `Get/SetClipboardText`/`ClearClipboard` in `WindowsPlatform.cpp`. Copy/cut/paste flow through the page's own Ctrl shortcuts — no WndProc/JS shim needed. | M |
| **WM_CHAR / WM_UNICHAR keyboard path** | 🟡 | Editing accelerators (Ctrl+A/C/V/X/Z) now work via the VK path — fixed 2026-06-24 by putting `unmodified_text` on the `RawKeyDown` (WebCore resolves shortcuts from it; without it every Ctrl-shortcut, and thus clipboard, was dead). Still open: text entry is US-ASCII synthesized from VK and WM_CHAR is **blocked** — adopt the OS char stream for accents/AltGr/non-US layouts (groundwork for IME). | L |
| **Per-view Show/Hide for config/declarative views** | ✅ | Done + verified in-game 2026-06-24. `setViewHidden` bridge command → `Runtime::SetViewHidden(id, hidden)` → renderer per-view `hidden` flag, independent of the global `_visible` toggle. Also fixed a latent compositor bug: hide/show now forces one recomposite (`compositeDirty`) so a *static* hidden view doesn't linger. ⚠ Same recomposite gap still affects API `SetViewOrder` (left out of scope). | M |
| **Load-lifecycle events to callers** (OnFinishLoading / OnFailLoading) | ✅ | Internal hook done + verified 2026-06-24: finish/fail routed worker→pump→`Runtime::OnViewLoad`, tracked per-view (`ViewLoadState` + `GetViewLoadState`) as the trigger for **P2 URL crash-recovery** (TODO marked there). Public consumer-API exposure **deliberately deferred** — would need a new V-interface to keep V1/V2 byte-compatible with Prisma. | M |
| **Prove FocusMenu + ship ControlLayer on-by-default** | 🔬 | `OSFUI→PrismaUISF_FocusMenu` engine IMenu + `ControlLayer` (BSInputEnableManager) are EXPERIMENTAL, off by default, **unproven on 1.16.244**. The gamepad leak (game still gets XInput while overlay open) is a real bug until ControlLayer is proven. See `docs/reverse-engineering-notes.md`. | L |
| **HDR / 10-bit / multi-format PSO + frame-gen swapchain** | ❌ | `D3D12Compositor.cpp` uses a single-format PSO; a 2nd format is skipped, and under DLSS-G/frame-gen the scanned-out swapchain isn't selected. **D3D12/Starfield-only cost PrismaUI never pays** — budget real time. | L |

---

## P2 — resilience & tooling

| Item | Status | Where / notes | Effort |
|---|---|---|---|
| **Per-view Destroy/teardown for config views** | 🟡 | Done for API views (`DestroyView`); config views only tear down at process exit. | M |
| **URL crash-recovery** | ❌ | No `originalUrl`/`needsRecovery`/reload-after-fault. Prisma auto-reloads a view after a renderer exception (`PrismaView` recovery fields). | M |
| **Native present-time DrawCursor sprite** | ❌ | No native cursor — the page draws a CSS pointer, so no cursor on a passive/crashed view. Engine cursor pos is reachable; draw a sprite in the compositor. Ref Prisma `ViewRenderer::DrawCursor` (cursor.png). | M |
| **DevTools / Inspector** | ❌ | The 4 `*Inspector*` API methods are **no-op stubs** today. Real impl needs a 2nd compositor layer + input routing (harder in D3D12 than Prisma's D3D11 SpriteBatch), and the remote inspector is gated behind an Ultralight **Pro** license. | L |
| **GPU-side N-texture z-order compositing** | ❌ | Multi-view is currently **CPU-blended** into one texture (`UltralightWebRenderer.cpp` CompositeViews); the SRV heap is sized for one texture. Move to GPU compositing if many/large overlapping views become common. | L |

---

## P3 — ergonomics & cleanup

| Item | Status | Where / notes | Effort |
|---|---|---|---|
| **NanoID-style handles** | ◆ | API handles are a monotonic counter (`ViewRegistry`); fine. Switch to random 64-bit only if collision-across-reload identity matters. | S |
| **Per-view focus contract for config views** | 🟡 | API `Focus`/`HasFocus` exist; the config-view `Tab`-cycle (`Runtime::CycleActiveView`) is a separate, overloaded mechanism. Unify if needed. | M |
| **Delete dead input scaffolding** | 🧹 | `src/input/InputRouter.*` ("NOTHING CALLS THIS YET") and the observe-only `UiInputHook` are unused; the live path is the WndProc subclass. Remove to cut confusion. | S |
| **Fix stale docs/comments** | 🧹 | README's "SFSE InputMap" claim (code uses raw VK); the "d3d12 is a stub" comments in `Config.h` / `Runtime.cpp` (the compositor is **not** a stub — verified in-game). | S |
| **Remote http(s) URL views** | ⛔ | Deliberate **non-goal** under the JS sandbox (`docs/security-model.md`); `file:///` only. Defer indefinitely unless the threat model changes. | — |

---

## Cross-cutting risks (could move estimates)

- **Custom IMenu / FocusMenu on 1.16.244** is a hand-built engine object
  (`uiMovie=null`), survival-past-first-open unresolved — may need a different RE
  approach, expanding P1.
- **D3D12 compositor** is production-shaped (verified 2026-06-12/13) but HDR
  backbuffers + frame-gen swapchain selection are untested (highest render
  uncertainty).
- **IME / Unicode** is greenfield (WM_CHAR currently blocked); any estimate is
  soft until the OS char path lands (P1).
- **Overlay coexistence** (ReShade / RTSS / Steam overlay / HDR) hook-chain
  ordering is untested.

## Non-code follow-ups (owner: you)
- **Branding/licensing:** using "PrismaUI" as the product name goes beyond
  StarkMP's permission to *reference* the brand — confirm with StarkMP and update
  the README affiliation language.
- **Repo directory** is still named `OSF UI` (only the build target is
  "PrismaUI SF"); rename the folder + GitHub repo if desired.
