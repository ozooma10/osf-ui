ScriptName __OSFUI_SCRIPT_NAME__ Hidden
{Recordless GLOBAL library called directly by JavaScript through OSF UI. This loose PEX needs no quest, plugin record, or registration.}

Function Refresh() Global
    bool actionsEnabled = OSFUI_Settings.GetBool("__OSFUI_MOD_ID__", "enabled", true)
    string greeting = OSFUI_Settings.GetString("__OSFUI_MOD_ID__", "greeting", "Hello from __OSFUI_SCRIPT_NAME__")
    OSFUI_View.SetState("__OSFUI_MOD_ID__", "greeting", greeting)
    OSFUI_View.SetState("__OSFUI_MOD_ID__", "clicks", 0)
    OSFUI_View.SetState("__OSFUI_MOD_ID__", "enabled", actionsEnabled)
EndFunction

; JavaScript: osfui.papyrus.call("__OSFUI_SCRIPT_NAME__", "Bump", total)
; The VIEW owns the running total and passes it in. A recordless GLOBAL script has nowhere to accumulate
Function Bump(int total) Global
    If !OSFUI_Settings.GetBool("__OSFUI_MOD_ID__", "enabled", true)
        Var[] disabledArgs = new Var[1]
        disabledArgs[0] = "Mod-backend actions are disabled in Mod Settings"
        OSFUI_View.EmitEvent("__OSFUI_MOD_ID__", "notice", disabledArgs)
        Return
    EndIf
    OSFUI_View.SetState("__OSFUI_MOD_ID__", "clicks", total)
    Var[] noticeArgs = new Var[1]
    noticeArgs[0] = "JavaScript called a GLOBAL Papyrus function"
    OSFUI_View.EmitEvent("__OSFUI_MOD_ID__", "notice", noticeArgs)
EndFunction

; Next steps:
;   - Real forms: OSFUI_View.SetState accepts one Form through Var, and SetStateForms publishes a Form array.
;     Runtime FormIDs are session-scoped, so never store a serialized identity across a save.
;   - Player-facing options belong in a settings schema and are available through OSFUI_Settings.GetBool/GetInt/GetString.
