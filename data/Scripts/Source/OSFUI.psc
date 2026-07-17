ScriptName OSFUI Native Hidden

; OSF UI - Papyrus surface for the shared settings platform (MCM).
;
; Your mod's settings live in a drop-in schema file:
;   Data/OSFUI/settings/<author>.<modname>.json
; (see docs/authoring-settings.md and examples/settings-only/).
; this script is how Papyrus reads them back, writes them, and reacts to changes and hotkey presses.
;
; If OSF UI is not installed, every call here fails soft: Papyrus logs a  missing-native error and the call yields the declared default (GetVersion() yields 0 - the feature-detect gate).
;
; Ids and keys are CASE-SENSITIVE and must match your schema file exactly (mod ids are lowercase "<author>.<modname>" by grammar).

; --- feature detect -----------------------------------------------------------

; Packed host version: major*10000 + minor*100 + patch (1.0.0 -> 10000).
; 0 => OSF UI absent.
int Function GetVersion() Global Native
; Human-readable host version ("1.0.0").
string Function GetVersionString() Global Native

; --- reading settings ---------------------------------------------------------
; Typed getters over the live value store. 
; Unknown mod/key, or a value whose type does not match the getter (e.g. GetInt on a float setting), yields the passed default. 
; Cheap and thread-safe; fine to call every time you need the value instead of caching.
bool Function GetBool(string asModId, string asKey, bool abDefault = false) Global Native
int Function GetInt(string asModId, string asKey, int aiDefault = 0) Global Native
float Function GetFloat(string asModId, string asKey, float afDefault = 0.0) Global Native

; Covers string-, enum-, and key-typed settings (enum yields the stored option value, key yields the key NAME, e.g. "F10").
string Function GetString(string asModId, string asKey, string asDefault = "") Global Native

; --- writing settings ---------------------------------------------------------
; Fire-and-forget: the write is queued, then validated/clamped against the schema and persisted by OSF UI on its next frame - exactly the same path as the settings menu. 
; A refused write (unknown key, wrong type) is logged to OSF UI's log and dropped. 
; An open settings menu updates live, and your registered change callback fires once the value commits.

Function SetBool(string asModId, string asKey, bool abValue) Global Native
Function SetInt(string asModId, string asKey, int aiValue) Global Native
Function SetFloat(string asModId, string asKey, float afValue) Global Native
Function SetString(string asModId, string asKey, string asValue) Global Native
; Restore schema defaults: one setting, or the whole mod when asKey is "".
Function Reset(string asModId, string asKey = "") Global Native

; --- change events ------------------------------------------------------------
; Calls akReceiver.asFn(string modId, string key) after any value of asModId commits (any writer: the settings menu, native code, Papyrus). 
; asModId "" subscribes to every mod. Returns a token (0 = failed).
;
; Function OnSettingChanged(string asModId, string asKey)   ; on akReceiver
;
; Registrations are SESSION-scoped: they do not survive a save load. 
; Re-register every time your script handles a game load (e.g. OnPlayerLoadGame on the player alias), like any other event registration.
int Function RegisterForSettingChanges(ScriptObject akReceiver, string asFn, string asModId = "") Global Native
; Instance-free variant for script LIBRARIES: dispatches to the GLOBAL function asScript.asFn(string, string). Same semantics/token as above.
int Function RegisterForSettingChangesStatic(string asScript, string asFn, string asModId = "") Global Native

; --- hotkeys ------------------------------------------------------------------
; A hotkey is a `"type": "key"` setting in your schema - the user sees and rebinds it in the settings menu like everything else, and OSF UI owns the input hook. 
; Registering delivers presses to your script:
;
;   Function OnHotkey(string asModId, string asKey)   ; on akReceiver
;
; asKey "" subscribes to every key-typed setting of asModId. 
; Presses are delivered during gameplay only - never while the user is typing into an overlay view or rebinding a key - and never consume the key. 
; Session-scoped, exactly like RegisterForSettingChanges.
int Function RegisterForHotkey(ScriptObject akReceiver, string asFn, string asModId, string asKey = "") Global Native
; Instance-free variant: GLOBAL function asScript.asFn(string, string).
int Function RegisterForHotkeyStatic(string asScript, string asFn, string asModId, string asKey = "") Global Native

; Release a RegisterFor* token. False on a stale/invalid token.
bool Function Unregister(int aiToken) Global Native

; --- menus --------------------------------------------------------------------
; Ask OSF UI to open/close an overlay view; "settings" is the Mods surface (same as F10), where your settings card lives. 
; Honored on OSF UI's next frame through its normal menu policy. False only on a malformed view id.
bool Function OpenMenu(string asViewId = "settings") Global Native
bool Function CloseMenu(string asViewId = "settings") Global Native
