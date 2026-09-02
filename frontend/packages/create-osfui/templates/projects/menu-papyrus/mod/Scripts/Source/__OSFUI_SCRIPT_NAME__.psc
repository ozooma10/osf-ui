ScriptName __OSFUI_SCRIPT_NAME__ Hidden
{Recordless GLOBAL library called directly by JavaScript through OSF UI. This loose PEX needs no quest, plugin record, or registration.}

Function Refresh() Global
    OSFUI_View.SetState("__OSFUI_MOD_ID__", "clicks", 0)
EndFunction

; JavaScript: osfui.send("papyrus.call", { script: "__OSFUI_SCRIPT_NAME__", function: "Bump", args: [total] })
; The VIEW owns the running total and passes it in. A recordless GLOBAL script has nowhere to accumulate
Function Bump(int total) Global
    OSFUI_View.SetState("__OSFUI_MOD_ID__", "clicks", total)
    Var[] noticeArgs = new Var[1]
    noticeArgs[0] = "JavaScript called a GLOBAL Papyrus function"
    OSFUI_View.EmitEvent("__OSFUI_MOD_ID__", "notice", noticeArgs)
EndFunction

; Next steps:
;   - Real forms: OSFUI_View.SetState accepts one Form through Var, and SetStateForms publishes a Form array. Runtime FormIDs are session-scoped, so never store a serialized identity across a save.
;   - Read player options through the independent OSF Settings Papyrus API, then explicitly forward only the values this view needs with SetState.
