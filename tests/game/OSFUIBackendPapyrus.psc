ScriptName OSFUIBackendPapyrus Hidden

Function Register() Global
    OSFUI.RegisterRequestStatic("OSFUIBackendPapyrus", "osfui-backend-test", "papyrus")
EndFunction

Function ObserveInventory() Global
    Debug.Trace("[OSFUIRealMods] inventoryCredits=" + Game.GetPlayer().GetItemCount(Game.GetForm(15)) + "; release=" + OSFUI.GetVersionString())
EndFunction

Function OnOSFUIRequest(string asName, Var[] akArgs, string asSourceViewId, string asReplyToken) Global
    If asName == "papyrus" && akArgs.Length == 1
        OSFUI.Reply(asReplyToken, (akArgs[0] as int) + 5)
    Else
        OSFUI.Reject(asReplyToken, "invalid-test", "Expected one integer")
    EndIf
EndFunction
