Scriptname OSFBrowserTV extends ObjectReference
{Craftable browser TV. The browser and its saved preferences belong to World Screens.}

Message Property WatchText Auto Const Mandatory

String screenHandle = ""
Bool attached = false
Int retries = 0
Int BIND_TIMER = 17730 Const
Guard BindingGuard ProtectsFunctionLogic
Guard ActivationGuard ProtectsFunctionLogic

Event OnInit()
    AttachScreen()
EndEvent

Event OnLoad()
    AttachScreen()
EndEvent

Event OnCellAttach()
    AttachScreen()
EndEvent

Event Actor.OnPlayerLoadGame(Actor akSender)
    ; Saved handles expire on load. BindScreen obtains this session's handle.
    AttachScreen()
EndEvent

Event OnWorkshopObjectPlaced(ObjectReference akReference)
    AttachScreen()
EndEvent

Event OnWorkshopObjectMoved(ObjectReference akReference)
    AttachScreen()
EndEvent

Event OnWorkshopObjectRemoved(ObjectReference akReference)
    ReleaseScreen()
EndEvent

Event OnUnload()
    ReleaseScreen()
EndEvent

Event OnCellDetach()
    ReleaseScreen()
EndEvent

Function AttachScreen()
    LockGuard BindingGuard
        attached = true
        retries = 0
        RegisterForRemoteEvent(Game.GetPlayer(), "OnPlayerLoadGame")
        SetActivateTextOverride(WatchText)
        RefreshBinding()
    EndLockGuard
EndFunction

Function ReleaseScreen()
    LockGuard BindingGuard
        attached = false
        CancelTimer(BIND_TIMER)
        UnregisterForRemoteEvent(Game.GetPlayer(), "OnPlayerLoadGame")
        OSFUIWorldScreens.UnbindScreen(screenHandle)
        screenHandle = ""
        retries = 0
    EndLockGuard
EndFunction

Function RefreshBinding() RequiresGuard(BindingGuard)
    if !attached || IsDeleted() || IsDisabled()
        return
    endif
    if Is3DLoaded()
        ; Idempotent for an existing binding: never unbind just to refresh.
        String bound = OSFUIWorldScreens.BindScreen(Self, "osfui.browser/tv")
        if bound != ""
            screenHandle = bound
            retries = 0
            CancelTimer(BIND_TIMER)
            return
        endif
    endif
    if retries < 10
        ; OnLoad can precede native material setup or completion of a save load.
        retries += 1
        StartTimer(1.0, BIND_TIMER)
    else
        Debug.Trace("[OSF Browser TV] Screen binding unavailable; activate to retry.")
    endif
EndFunction

Event OnTimer(Int aiTimerID)
    if aiTimerID == BIND_TIMER
        LockGuard BindingGuard
            RefreshBinding()
        EndLockGuard
    endif
EndEvent

Event OnActivate(ObjectReference akActionRef)
    if akActionRef != Game.GetPlayer()
        return
    endif
    TryLockGuard ActivationGuard
        LockGuard BindingGuard
            if !attached || !Is3DLoaded() || IsDeleted() || IsDisabled()
                return
            endif
            retries = 0
            RefreshBinding()
            if screenHandle == "" || !OSFUIWorldScreens.OpenFullscreen(screenHandle)
                Debug.Notification("The TV is not ready. Try again in a moment.")
            endif
        EndLockGuard
    EndTryLockGuard
EndEvent
