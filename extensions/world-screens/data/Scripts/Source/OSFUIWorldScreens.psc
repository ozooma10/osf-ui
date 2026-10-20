Scriptname OSFUIWorldScreens Native Hidden

; Handles are session-scoped capabilities. Save the selected qualified view ID,
; then rebind after load/cell attachment; unbind on unload/detachment.
; Empty means the reference's private geometry/material is not ready or supported.
String Function BindScreen(ObjectReference reference, String viewId) Global Native
Bool Function UnbindScreen(String handle) Global Native

; Requests fullscreen interaction (cursor + pause, the screen's page over the game) for a
; bound screen whose view manifest sets "fullscreen": true. True means queued; admission can
; still be rejected. Esc, pad Back, focus loss, or opening a menu ends it.
Bool Function OpenFullscreen(String handle) Global Native
