ScriptName OSFUIHotkeySample Hidden

; Minimal schema-owned hotkey target. No quest, alias, or OSFUI registration is
; needed: OSF UI invokes this GLOBAL function directly from the installed
; settings schema after its normal gameplay input gates pass.
Function OnHotkey(string asModId, string asKey) Global
    string notice = "OSF UI onPress: " + asModId + "." + asKey
    Debug.Trace("[OSFUIHotkeySample] " + notice)
    Debug.Notification(notice)
EndFunction
