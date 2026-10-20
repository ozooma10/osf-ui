Scriptname OSFFullscreenProbe Hidden
; Manual fullscreen acceptance only. Never package with the extension.
; Uses Starcade's registered Red Mile screen and screen-play material.

Function OpenRedMile() Global
    Open(0x000B9FCD)
EndFunction

Function Open(int referenceId) Global
    ObjectReference screen = Game.GetForm(referenceId) as ObjectReference
    string handle = OSFUIWorldScreens.BindScreen(screen, "starcade.arcade/screen-play")
    bool queued = false
    if handle != ""
        ; The console is a blocking menu. Utility.Wait counts only outside menu
        ; mode, so admission happens after the user closes it and releases input.
        Utility.Wait(1.0)
        queued = OSFUIWorldScreens.OpenFullscreen(handle)
    endif
    Debug.Trace("[OSFFullscreenProbe] reference=" + screen + " bound=" + (handle != "") + " queued=" + queued)
EndFunction
