Scriptname OSFWorldScreenProbe Hidden
; Private game-test script. Never packaged in the extension or content addons.

Function PlayRedMile() Global
    ObjectReference screen = Game.GetForm(0x000B9FCD) as ObjectReference
    bool refreshed = x2357StarcadeNative.RefreshScreen(screen, "orbital-paddle")
    bool playing = x2357StarcadeNative.PlayScreen(screen, "orbital-paddle")
    Debug.Trace("[WorldScreenProbe] play refreshed=" + refreshed + " accepted=" + playing)
EndFunction

Function SpawnVoss() Global
    Actor player = Game.GetPlayer()
    ObjectReference screen = player.PlaceAtMe(Game.GetFormFromFile(0x802, "x2357ssse-key-screens.esm"))
    screen.MoveTo(player, 0.0, 2.0, 1.3)
    screen.SetAngle(0.0, 0.0, 0.0)
    Debug.Trace("[WorldScreenProbe] new Voss reference=" + screen)
EndFunction

Function EndRedMile() Global
    ; Teardown test: release the reference binding. Normal page/Escape exit is
    ; a separate interaction path and is not simulated by this function.
    ObjectReference screen = Game.GetForm(0x000B9FCD) as ObjectReference
    x2357StarcadeNative.ReleaseScreen(screen)
    Debug.Trace("[WorldScreenProbe] screen binding released")
EndFunction

Function SpawnDisplays() Global
    Actor player = Game.GetPlayer()
    ObjectReference arcade = player.PlaceAtMe(Game.GetFormFromFile(0x800, "x2357starcade-world.esm"))
    ObjectReference market = player.PlaceAtMe(Game.GetFormFromFile(0x800, "x2357ssse-displays.esm"))
    arcade.MoveTo(player, -2.0, 3.0, 0.0)
    market.MoveTo(player, 2.0, 3.0, 0.0)
    Debug.Trace("[WorldScreenProbe] spawned arcade=" + arcade + " market=" + market)
EndFunction
