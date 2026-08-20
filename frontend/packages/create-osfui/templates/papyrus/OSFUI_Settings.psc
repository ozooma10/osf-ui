ScriptName OSFUI_Settings Native Hidden

; OSF UI settings and hotkey API.
;
; Settings are declared in:
;   Data/SFSE/Plugins/OSFUI/settings/<modId>.json
;
; Mod ids, keys, and enum option values are matched case-insensitively.
; Registrations are SESSION-scoped: register again after every game load and never save a registration token.

; =============================================================================
; Read
; =============================================================================

; Unknown settings and type mismatches return the caller-provided default.
bool Function GetBool(string asModId, string asKey, bool abDefault = false) Global Native
int Function GetInt(string asModId, string asKey, int aiDefault = 0) Global Native
float Function GetFloat(string asModId, string asKey, float afDefault = 0.0) Global Native

; Covers string-, enum-, and key-typed settings. Enum settings return the stored option value; key settings return the current key name.
string Function GetString(string asModId, string asKey, string asDefault = "") Global Native

; =============================================================================
; Write
; =============================================================================

; Writes are queued, then validated and clamped against the installed schema.
; True means the operation was admitted to the queue, not that it has already committed. Refused writes are logged and leave the stored value unchanged.
bool Function SetBool(string asModId, string asKey, bool abValue) Global Native
bool Function SetInt(string asModId, string asKey, int aiValue) Global Native
bool Function SetFloat(string asModId, string asKey, float afValue) Global Native
bool Function SetString(string asModId, string asKey, string asValue) Global Native

; Reset one setting, or every setting for the mod when asKey is empty.
bool Function Reset(string asModId, string asKey = "") Global Native

; =============================================================================
; Change listeners
; =============================================================================

; Receiver callback:
;   Function OnOSFUISettingChanged(string asModId, string asKey)
;
; Empty asKey listens to every setting in asModId. Returns a session-scoped registration token, or 0 on failure.
int Function ListenForChanges(ScriptObject akReceiver, string asModId, string asKey = "") Global Native

; GLOBAL-function variant. Invokes OnOSFUISettingChanged on asScript.
int Function ListenForChangesStatic(string asScript, string asModId, string asKey = "") Global Native


; =============================================================================
; Hotkeys
; =============================================================================

; Receiver callback:
;   Function OnOSFUIHotkey(string asModId, string asKey)
;
; Empty asKey listens to every key-typed setting in asModId. Hotkeys fire only during gameplay, never while an OSF UI view is accepting text/key input.
int Function ListenForHotkeys(ScriptObject akReceiver, string asModId, string asKey = "") Global Native

; GLOBAL-function variant. Invokes OnOSFUIHotkey on asScript.
int Function ListenForHotkeysStatic(string asScript, string asModId, string asKey = "") Global Native


; Remove a settings or hotkey registration. Returns false for 0 or a stale or session-expired token.
bool Function Unregister(int aiRegistrationToken) Global Native
