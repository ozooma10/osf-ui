ScriptName OSFUI Native Hidden

; OSF UI runtime API.
;
; Settings are provided by the separate OSF Settings product.
; JavaScript view communication and presentation live in OSFUI_View.psc.

; True when the OSF UI native runtime is available.
bool Function IsAvailable() Global Native

; Packed release version: major*10000 + minor*100 + patch.
int Function GetVersion() Global Native
; Human-readable release version, for display and diagnostics only.
string Function GetVersionString() Global Native
