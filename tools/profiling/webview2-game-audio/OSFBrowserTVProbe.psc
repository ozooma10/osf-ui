Scriptname OSFBrowserTVProbe Hidden
; Private setup only. Watch is dispatched through the real reference activation.
Function Spawn() Global
    Actor player = Game.GetPlayer()
    Form base = Game.GetFormFromFile(0x800, "OSFBrowserTV.esm")
    ObjectReference screen = player.PlaceAtMe(base)
    player.SetAngle(0.0, 0.0, 0.0)
    screen.MoveTo(player, 0.0, 2.5, 1.5)
    screen.SetAngle(0.0, 0.0, 0.0)
    Debug.Trace("[BrowserTVProbe] spawned=" + screen + " id=" + screen.GetFormID())
EndFunction

Function Watch(Int formId) Global
    ObjectReference screen = Game.GetForm(formId) as ObjectReference
    Bool accepted = screen.Activate(Game.GetPlayer())
    Debug.Trace("[BrowserTVProbe] Activate=" + accepted + " screen=" + screen)
EndFunction

Function SpawnAt(Float x, Float y, Float z) Global
    Actor player = Game.GetPlayer()
    Form base = Game.GetFormFromFile(0x800, "OSFBrowserTV.esm")
    ObjectReference screen = player.PlaceAtMe(base)
    screen.MoveTo(player, x, y, z)
    screen.SetAngle(0.0, 0.0, 0.0)
    Debug.Trace("[BrowserTVProbe] second=" + screen + " id=" + screen.GetFormID())
EndFunction
