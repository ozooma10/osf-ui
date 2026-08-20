ScriptName OSFUI Native Hidden

; OSF UI runtime API.
;
; Settings live in OSFUI_Settings.psc;
; JavaScript view communication and presentation live in OSFUI_View.psc.

; True when the OSF UI native runtime is available.
bool Function IsAvailable() Global Native

; Packed release version: major*10000 + minor*100 + patch.
; Returns 0 when OSF UI is unavailable.
int Function GetVersion() Global Native

; Human-readable release version, for display and diagnostics only.
string Function GetVersionString() Global Native
