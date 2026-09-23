# Forwarded WebView2 input prototype

This prototype keeps native keyboard focus on Starfield while an OSF UI menu is
open. WebView2 still runs in the helper process and uses the existing composition
controller, capture, shared textures, and D3D12 compositor. It changes input
ownership rather than moving Chromium into the game process.

## Input path

- The game window forwards physical key down/up events, including repeats,
  modifiers, physical key codes, and layout-dependent key names.
- The helper sends `Input.dispatchKeyEvent` through
  `ICoreWebView2::CallDevToolsProtocolMethod`. Chromium performs normal editing
  actions such as Backspace, Tab, and Ctrl+A.
- `WM_CHAR` supplies translated text, including dead-key results and UTF-16
  surrogate pairs. Key events do not also insert text. IME composition and commits
  use `Input.imeSetComposition` and `Input.insertText`.
- Mouse events still use `ICoreWebView2CompositionController::SendMouseInput`.
  Button state and Shift/Ctrl are included for dragging and modified clicks.
- The helper stays under its offscreen `WS_EX_NOACTIVATE` owner. It does not
  reparent into Starfield, call `MoveFocus`, register its own raw mouse input,
  subclass Chromium's input widget, or acquire native mouse capture.
- Input is admitted only after the game's focus menu is ready, and only while
  Starfield is foreground. Hiding a view, changing the input target, revoking the
  grant, or losing game focus releases held keys and cancels browser composition.
- Existing synthetic controller navigation remains a separate bridge message.
  Escape and developer F12 remain framework-owned.

CDP calls are serialized per view, including focus emulation and key releases.
The queue is capped at 256 commands and fails after a command stalls for five
seconds. A failure enters the existing browser-host recovery path. Closing or
navigating a view discards its pending commands; late callbacks cannot dispatch
more input from the discarded queue. Physical input resumes after the new
document loads. An already-dispatched command cannot be recalled.

## Trying it

Forwarded input is the sole input path. Deploy the rebuilt DLL and helper together;
this change uses IPC protocol 16. Restart Starfield, open an interactive view, and look
for `WebView2 input mode: forwarded CDP` in the native log or
`input mode: forwarded CDP` in `OSF UI.webview2-host.log`. The helper logs the first
physical keyboard event and the first translated text event without logging their
contents.

## Verification and remaining checks

The prototype build passed all 29 native test suites and the real WebView2 smoke
test on Windows. Both the rebuilt DLL and helper were deployed to the MO2
`OSF UI` mod and verified against their build-output SHA-256 hashes.

Automated native tests cover command order, failure, timeout, overflow, close with
a pending callback, key release tracking, message round trips, and Unicode
boundaries. The Windows `wv2-cdp-smoke` target creates a real composition WebView2
controller and checks text, Unicode, Backspace repeat, Ctrl+A, Tab, and IME protocol
commands while asserting that the test process did not acquire native focus.

```powershell
xmake build wv2-cdp-smoke
& .\build\windows\x64\releasedbg\wv2-cdp-smoke.exe
```

The standalone test does not exercise Starfield's message pump, rendering, or a
physical IME. Typing in the example view's input field worked in-game before the
native-path cleanup; the deployed cleanup build still needs an in-game check:

1. Type in a text field, including punctuation, shifted characters, and a non-US
   layout. Check arrows, held Backspace, Ctrl+A, Ctrl+C/V, Tab, and Shift+Tab.
2. Drag a slider or text selection, scroll, and try Shift/Ctrl clicks. Close and
   reopen while holding a key or mouse button; verify no input remains held.
3. Alt+Tab away while typing or dragging, then return. Confirm the game does not
   fight the other application's focus and resumes input on return.
4. Check controller navigation and relative-pointer interactions. Repeatedly switch views and reopen menus.
5. Check a physical IME's preedit, commit, cancellation, and candidate window.

Known limits of the prototype:

- Disabling `Emulation.setFocusEmulationEnabled` does not reliably make
  `document.hasFocus()` false. Native input gating and explicit key releases are
  the deactivation mechanism; DOM focus/blur events are not an ownership signal.
- OS IME candidate positioning is not connected to the DOM caret. Native browser
  popups, clipboard shortcuts, and accessibility behavior need live validation.
- Physical keyboard/text delivery relies on Starfield delivering `WM_KEY*` and
  `WM_CHAR` to the hooked window. The runtime smoke test validates Chromium's CDP
  behavior, not that game-side delivery. The first-event logs distinguish these.
- Keyboard/text CDP commands are ordered with each other. Mouse input uses the
  separate composition API, so very fast mixed mouse/keyboard sequences also
  deserve testing.
