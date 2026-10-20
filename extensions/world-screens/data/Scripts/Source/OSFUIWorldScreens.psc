Scriptname OSFUIWorldScreens Native Hidden

; Handles are session-scoped capabilities. Save the selected qualified view ID,
; then rebind after load/cell attachment; unbind on unload/detachment.
; Empty means the reference's private geometry/material is not ready or supported.
String Function BindScreen(ObjectReference reference, String viewId) Global Native
Bool Function UnbindScreen(String handle) Global Native
