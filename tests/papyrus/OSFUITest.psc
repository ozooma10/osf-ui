ScriptName OSFUITest Hidden

; Console-driven validation of the split OSF UI Papyrus API (OSF UI repo,
; tests/papyrus/). Needs osfui.paptest.json deployed to
; SFSE/Plugins/OSFUI/settings/ and this script's pex in Data/Scripts/.
;
;   cgf "OSFUITest.RunAll"      full getter/setter/clamp/reset suite; also
;                               registers settings + hotkey callbacks and
;                               publishes one retained state/event smoke
;   cgf "OSFUITest.HookSettings" register just settings + hotkey callbacks
;   cgf "OSFUITest.HookView"    register testSend + testRequest endpoints
;   cgf "OSFUITest.DropSettings" <tok> unregister a settings/hotkey token
;   cgf "OSFUITest.DropView" <tok> unregister a view endpoint token
;   cgf "OSFUITest.OpenSettings" / "OSFUITest.CloseSettings"
;   press F8 (testHotkey)       in gameplay -> HUD notification from OnOSFUIHotkey
;
; Results land as HUD notifications (summary + failures) and [OSFUITest]
; lines in the Papyrus user log (enable [Papyrus] bEnableLogging=1). Each
; RunAll/HookSettings adds ANOTHER callback registration (no state to dedupe in a
; globals-only script) - duplicate change traces after a re-run are expected,
; or save-load to clear (registrations are session-scoped).

string Function ModId() Global
	return "osfui.paptest"
EndFunction

int Function Check(bool abOk, string asWhat, int aiFails) Global
	If abOk
		Debug.Trace("[OSFUITest] PASS " + asWhat)
		return aiFails
	EndIf
	Debug.Trace("[OSFUITest] FAIL " + asWhat, 2)
	Debug.Notification("OSFUITest FAIL: " + asWhat)
	return aiFails + 1
EndFunction

bool Function Near(float afA, float afB) Global
	float d = afA - afB
	return d > -0.001 && d < 0.001
EndFunction

Function HookSettings() Global
	int t1 = OSFUI_Settings.ListenForChangesStatic("OSFUITest", ModId())
	int t2 = OSFUI_Settings.ListenForHotkeysStatic("OSFUITest", ModId())
	Debug.Trace("[OSFUITest] callback tokens: settings=" + t1 + " hotkey=" + t2)
	If t1 == 0 || t2 == 0
		Debug.Notification("OSFUITest: callback registration FAILED")
	Else
		Debug.Notification("OSFUITest: callbacks registered - press the test hotkey (default F8)")
	EndIf
EndFunction

Function OnOSFUISettingChanged(string asModId, string asKey) Global
	Debug.Trace("[OSFUITest] OnOSFUISettingChanged " + asModId + "." + asKey)
EndFunction

Function OnOSFUIHotkey(string asModId, string asKey) Global
	Debug.Trace("[OSFUITest] OnOSFUIHotkey " + asModId + "." + asKey)
	Debug.Notification("OSFUITest: hotkey fired (" + asKey + ")")
EndFunction

Function HookView() Global
	int sendToken = OSFUI_View.RegisterSendStatic("OSFUITest", ModId(), "testSend")
	int requestToken = OSFUI_View.RegisterRequestStatic("OSFUITest", ModId(), "testRequest")
	Debug.Trace("[OSFUITest] view tokens: send=" + sendToken + " request=" + requestToken)
	If sendToken == 0 || requestToken == 0
		Debug.Notification("OSFUITest: view endpoint registration FAILED")
	Else
		Debug.Notification("OSFUITest: testSend + testRequest registered")
	EndIf
EndFunction

Function OnOSFUISend(string asName, Var[] akArgs, string asSourceViewId) Global
	Debug.Trace("[OSFUITest] OnOSFUISend " + asName + " from " + asSourceViewId + " args=" + akArgs.Length)
EndFunction

Function OnOSFUIRequest(string asName, Var[] akArgs, string asSourceViewId, string asReplyToken) Global
	Debug.Trace("[OSFUITest] OnOSFUIRequest " + asName + " from " + asSourceViewId + " args=" + akArgs.Length)
	If asName == "testRequest"
		If akArgs.Length > 0
			OSFUI_View.Reply(asReplyToken, akArgs[0])
		Else
			OSFUI_View.Reply(asReplyToken, "papyrus reply")
		EndIf
	Else
		OSFUI_View.Reject(asReplyToken, "unknown-request", "OSFUITest does not handle " + asName)
	EndIf
EndFunction

Function DropSettings(int aiToken) Global
	bool ok = OSFUI_Settings.Unregister(aiToken)
	Debug.Trace("[OSFUITest] OSFUI_Settings.Unregister(" + aiToken + ") = " + ok)
	Debug.Notification("OSFUITest: settings unregister(" + aiToken + ") = " + ok)
EndFunction

Function DropView(int aiToken) Global
	bool ok = OSFUI_View.Unregister(aiToken)
	Debug.Trace("[OSFUITest] OSFUI_View.Unregister(" + aiToken + ") = " + ok)
	Debug.Notification("OSFUITest: view unregister(" + aiToken + ") = " + ok)
EndFunction

bool Function OpenSettings() Global
	return OSFUI_View.Open()
EndFunction

bool Function CloseSettings() Global
	return OSFUI_View.Close()
EndFunction

int Function CheckViewOutbound(int aiFails) Global
	string modId = ModId()
	Var[] args = new Var[2]
	args[0] = "papyrus"
	args[1] = 1
	bool stateQueued = OSFUI_View.SetState(modId, "smoke", "ready")
	bool eventQueued = OSFUI_View.EmitEvent(modId, "smoke", args)
	aiFails = Check(stateQueued, "OSFUI_View.SetState queued", aiFails)
	aiFails = Check(eventQueued, "OSFUI_View.EmitEvent queued", aiFails)
	return aiFails
EndFunction

Function RunAll() Global
	string m = ModId()
	Debug.Trace("[OSFUITest] === RunAll ===")
	int fails = 0

	; feature detect
	bool available = OSFUI.IsAvailable()
	fails = Check(available, "OSFUI.IsAvailable", fails)
	int v = OSFUI.GetVersion()
	fails = Check(v >= 10000, "GetVersion >= 10000 (got " + v + ")", fails)
	If !available || v == 0
		Debug.Notification("OSFUITest: OSF UI absent (GetVersion 0) - aborting")
		return
	EndIf
	fails = Check(OSFUI.GetVersionString() != "", "GetVersionString non-empty (got " + OSFUI.GetVersionString() + ")", fails)

	; callbacks first, so every commit below also produces an OnOSFUISettingChanged trace
	HookSettings()

	; start from schema defaults (writes commit on OSF UI's next frame -> wait)
	fails = Check(OSFUI_Settings.Reset(m), "Reset(mod) queued", fails)
	Utility.Wait(0.5)
	fails = Check(OSFUI_Settings.GetBool(m, "enabled", false) == true, "GetBool default true", fails)
	fails = Check(OSFUI_Settings.GetInt(m, "count", -1) == 3, "GetInt default 3", fails)
	fails = Check(Near(OSFUI_Settings.GetFloat(m, "scale", -1.0), 1.0), "GetFloat default 1.0", fails)
	fails = Check(OSFUI_Settings.GetString(m, "mode", "?") == "balanced", "GetString enum default 'balanced'", fails)
	fails = Check(OSFUI_Settings.GetString(m, "label", "?") == "hello", "GetString default 'hello'", fails)
	fails = Check(OSFUI_Settings.GetString(m, "testHotkey", "?") == "F8", "GetString key default 'F8'", fails)

	; misses yield the caller's default
	fails = Check(OSFUI_Settings.GetInt(m, "scale", -1) == -1, "type mismatch yields default", fails)
	fails = Check(OSFUI_Settings.GetBool(m, "nope", false) == false, "unknown key yields default", fails)
	fails = Check(OSFUI_Settings.GetInt("no.such", "count", 42) == 42, "unknown mod yields default", fails)

	; writes
	fails = Check(OSFUI_Settings.SetBool(m, "enabled", false), "SetBool queued", fails)
	fails = Check(OSFUI_Settings.SetInt(m, "count", 7), "SetInt queued", fails)
	fails = Check(OSFUI_Settings.SetFloat(m, "scale", 0.25), "SetFloat queued", fails)
	fails = Check(OSFUI_Settings.SetString(m, "mode", "fast"), "SetString enum queued", fails)
	fails = Check(OSFUI_Settings.SetString(m, "label", "papyrus was here"), "SetString text queued", fails)
	Utility.Wait(0.5)
	fails = Check(OSFUI_Settings.GetBool(m, "enabled", true) == false, "SetBool committed", fails)
	fails = Check(OSFUI_Settings.GetInt(m, "count", -1) == 7, "SetInt committed", fails)
	fails = Check(Near(OSFUI_Settings.GetFloat(m, "scale", -1.0), 0.25), "SetFloat committed", fails)
	fails = Check(OSFUI_Settings.GetString(m, "mode", "?") == "fast", "SetString enum committed", fails)
	fails = Check(OSFUI_Settings.GetString(m, "label", "?") == "papyrus was here", "SetString committed", fails)

	; clamping + refusal (same validation as the settings menu)
	OSFUI_Settings.SetInt(m, "count", 999)
	OSFUI_Settings.SetFloat(m, "scale", -5.0)
	OSFUI_Settings.SetString(m, "mode", "bogus")
	Utility.Wait(0.5)
	fails = Check(OSFUI_Settings.GetInt(m, "count", -1) == 10, "SetInt clamped to max 10", fails)
	fails = Check(Near(OSFUI_Settings.GetFloat(m, "scale", -1.0), 0.0), "SetFloat clamped to min 0", fails)
	fails = Check(OSFUI_Settings.GetString(m, "mode", "?") == "fast", "invalid enum refused (kept 'fast')", fails)

	; single-key reset, then whole-mod reset
	fails = Check(OSFUI_Settings.Reset(m, "count"), "Reset(key) queued", fails)
	Utility.Wait(0.5)
	fails = Check(OSFUI_Settings.GetInt(m, "count", -1) == 3, "Reset(key) restored default", fails)
	fails = Check(OSFUI_Settings.Reset(m), "Reset(mod) queued again", fails)
	Utility.Wait(0.5)
	fails = Check(OSFUI_Settings.GetBool(m, "enabled", false) == true, "Reset(mod) restored defaults", fails)

	; backend -> view contract accepts work even when no test view is instantiated
	fails = CheckViewOutbound(fails)

	If fails == 0
		Debug.Trace("[OSFUITest] === ALL CHECKS PASSED ===")
		Debug.Notification("OSFUITest: ALL CHECKS PASSED - now press F8, then check the settings menu")
	Else
		Debug.Trace("[OSFUITest] === " + fails + " CHECK(S) FAILED ===", 2)
		Debug.Notification("OSFUITest: " + fails + " CHECK(S) FAILED - see Papyrus log")
	EndIf
EndFunction
