Scriptname OSFCEFAudioProbe Hidden

; Disposable test fixture only. No engine pointer reads from a worker thread.
; Listener is the player's head position/yaw, not the final rendered camera.
Function Track(Int formId, Int seconds = 120, Float enabled = 1.0) Global
    ObjectReference tv = Game.GetForm(formId) as ObjectReference
    Actor player = Game.GetPlayer()
    Int sequence = 0
    While tv && player && sequence < seconds * 10
        Var[] pose = new Var[9]
        pose[0] = player.GetPositionX()
        pose[1] = player.GetPositionY()
        pose[2] = player.GetPositionZ() + 1.6
        pose[3] = player.GetAngleZ()
        pose[4] = tv.GetPositionX()
        pose[5] = tv.GetPositionY()
        pose[6] = tv.GetPositionZ()
        pose[7] = enabled
        pose[8] = sequence
        OSFUI.EmitEvent("osfui.browser", "cef-audio-pose", pose)
        sequence += 1
        Utility.WaitMenuPause(0.1)
    EndWhile
EndFunction
