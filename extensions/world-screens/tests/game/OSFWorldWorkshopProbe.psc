Scriptname OSFWorldWorkshopProbe Hidden
; Private setup/observation only. Placement is performed through the real workshop UI.

Function Supply() Global
    Actor player = Game.GetPlayer()
    ; Recipe FVPA references components; inventory needs the corresponding MISC forms.
    player.AddItem(Game.GetForm(0x00246B65), 50, true)
    player.AddItem(Game.GetForm(0x000057D9), 50, true)
    player.AddItem(Game.GetForm(0x0000557D), 50, true)
    Debug.Trace("[WorldWorkshopProbe] resources wire=" + player.GetItemCount(Game.GetForm(0x00246B65)) + " beryllium=" + player.GetItemCount(Game.GetForm(0x000057D9)) + " aluminum=" + player.GetItemCount(Game.GetForm(0x0000557D)))
EndFunction

Function GoToShip() Global
    SQ_PlayerShipScript shipQuest = Game.GetForm(0x000174A2) as SQ_PlayerShipScript
    ObjectReference workshop = shipQuest.PlayerShipWorkshop.GetReference()
    ObjectReference seat = shipQuest.PlayerShipPilotSeat.GetReference()
    Debug.Trace("[WorldWorkshopProbe] workshop=" + workshop + " seat=" + seat)
    if seat
        Game.GetPlayer().MoveTo(seat, 0.0, -1.5, 0.0)
    elseif workshop
        Game.GetPlayer().MoveTo(workshop)
    endif
EndFunction
