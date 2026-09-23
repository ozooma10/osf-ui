# Forwarded input

Starfield keeps native keyboard focus while a menu view is open. WebView2 stays in the helper process with the existing composition controller, capture, shared textures, and D3D12 compositor; only input ownership changes.

## Path

- The game window forwards physical key down/up (repeats, modifiers, physical codes, layout key names). The helper dispatches them with `Input.dispatchKeyEvent` over `ICoreWebView2::CallDevToolsProtocolMethod`, so Chromium handles Backspace, Tab, and Ctrl+A itself.
- `WM_CHAR` supplies text (dead keys, surrogate pairs) via `Input.insertText`; IME uses `Input.imeSetComposition`. Key events never also insert text.
- Mouse uses `ICoreWebView2CompositionController::SendMouseInput` with button state and Shift/Ctrl.
- The helper stays under its offscreen `WS_EX_NOACTIVATE` owner: no reparenting, `MoveFocus`, raw input, widget subclassing, or mouse capture.
- Input is admitted only once the focus menu is ready and while Starfield is foreground. Hiding a view, changing target, revoking the grant, or losing focus releases held keys and cancels composition.
- Controller navigation stays a separate bridge message. Escape and F12 (developer) stay framework-owned.

CDP calls are serialized per view, capped at 256 queued commands, and fail into browser-host recovery after a five-second stall. Closing or navigating a view discards pending commands; input resumes when the new document loads.

## Check it

Deploy the DLL and helper together and restart. The native log prints `WebView2 input mode: forwarded CDP`; `OSF UI.webview2-host.log` prints `input mode: forwarded CDP` and logs the first key and first text event without contents.

```powershell
xmake build wv2-cdp-smoke
& .\build\windows\x64\releasedbg\wv2-cdp-smoke.exe
```

The smoke test drives a real composition controller through text, Unicode, Backspace repeat, Ctrl+A, Tab, and IME commands and asserts the process never took native focus. Native tests cover ordering, failure, timeout, overflow, close with a pending callback, key release tracking, round trips, and Unicode boundaries. Neither exercises Starfield's message pump, rendering, or a physical IME.

In-game:

1. Type punctuation, shifted characters, and a non-US layout; arrows, held Backspace, Ctrl+A/C/V, Tab, Shift+Tab.
2. Drag a slider and a selection, scroll, Shift/Ctrl click. Close and reopen while holding a key or button; nothing stays held.
3. Alt+Tab away mid-typing or mid-drag and return; the game must not fight the other window's focus.
4. Controller navigation, relative pointer, repeated view switches and reopens.
5. Physical IME preedit, commit, cancel, and candidate window.

## Limits

- Disabling `Emulation.setFocusEmulationEnabled` does not reliably clear `document.hasFocus()`; native gating and key releases are the deactivation mechanism, not DOM focus events.
- The IME candidate window is not positioned at the DOM caret. Browser popups, clipboard, and accessibility need live checks.
- Delivery depends on Starfield passing `WM_KEY*` and `WM_CHAR` to the hooked window; the smoke test validates only Chromium's side. The first-event logs tell the two apart.
- Keyboard/text commands are ordered with each other but not with mouse input, which uses the composition API.
