ScriptName __OSFUI_SCRIPT_NAME__ Hidden
{Recordless GLOBAL library called directly by JavaScript through OSF UI. This loose PEX needs no quest, plugin record, or registration.}

Function Refresh() Global
    bool actionsEnabled = OSFUI.GetBool("__OSFUI_MOD_ID__", "enabled", true)
    string greeting = OSFUI.GetString("__OSFUI_MOD_ID__", "greeting", "Hello from __OSFUI_SCRIPT_NAME__")
    OSFUI.SetViewString("__OSFUI_MOD_ID__", "greeting", greeting)
    OSFUI.SetViewInt("__OSFUI_MOD_ID__", "clicks", 0)
    OSFUI.SetViewBool("__OSFUI_MOD_ID__", "enabled", actionsEnabled)
EndFunction

; JavaScript: osfui.papyrus.call("__OSFUI_SCRIPT_NAME__", "Bump", total)
; The VIEW owns the running total and passes it in. A recordless GLOBAL script has nowhere to accumulate
Function Bump(int total) Global
    If !OSFUI.GetBool("__OSFUI_MOD_ID__", "enabled", true)
        string[] disabledArgs = new string[1]
        disabledArgs[0] = "Mod-backend actions are disabled in Mod Settings"
        OSFUI.SendViewEvent("__OSFUI_MOD_ID__", "notice", disabledArgs)
        Return
    EndIf
    OSFUI.SetViewInt("__OSFUI_MOD_ID__", "clicks", total)
    string[] noticeArgs = new string[1]
    noticeArgs[0] = "JavaScript called a GLOBAL Papyrus function"
    OSFUI.SendViewEvent("__OSFUI_MOD_ID__", "notice", noticeArgs)
EndFunction

Function OpenSettings() Global
    OSFUI.OpenMenu()
EndFunction

Function Greet(string who) Global
    string greeting = OSFUI.GetString("__OSFUI_MOD_ID__", "greeting", "Hello from __OSFUI_SCRIPT_NAME__")
    OSFUI.SetViewString("__OSFUI_MOD_ID__", "greeting", greeting + ", " + who)
EndFunction

; Next steps:
;   - Real forms: OSFUI.SetViewForms publishes them as { formId, formType, name }, and OSFUI.GetFormById(formId) resolves one the view echoed back.
;     Runtime FormIDs are session-scoped - check the result for None before acting on it, and never store one across a save.
;   - Player-facing options belong in a settings schema and are available here through OSFUI.GetBool/GetInt/GetString.
